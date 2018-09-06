/*
 * host_resolver_tenta.cc
 *
 *  Created on: Dec 20, 2016
 *      Author: iotto
 */

#include <xwalk/runtime/browser/android/net/host_resolver_tenta.h>

#include <string>
#include <jni.h>

#include "base/logging.h"
#include "base/stl_util.h"
#include "base/android/jni_string.h"
#include "base/memory/ptr_util.h"
#include "base/time/time.h"
#include "net/dns/dns_util.h"
#include "net/base/net_errors.h"
#include "net/base/address_list.h"
#include "net/log/net_log_with_source.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/worker_pool.h"

#include "xwalk/runtime/browser/android/scoped_allow_wait_for_legacy_web_view_api.h"

#include "jni/HostResolverTenta_jni.h"

using base::android::AttachCurrentThread;
using base::android::ConvertUTF8ToJavaString;

namespace xwalk
{
namespace tenta
{

using namespace net;

/**********************************
 * class SavedRequest
 */
class HostResolverTenta::SavedRequest
{
public:
  SavedRequest(base::TimeTicks when_created, const RequestInfo& info,
               RequestPriority priority,
               const CompletionCallback& callback,
               AddressList* addresses)
    : info_(info),
      priority_(priority),  // not used
      callback_(callback),
      addresses_(addresses),
      sent_to_java_(false)
  {
    when_created_ = when_created;
  }

  /**
   * Notify callback of current status
   */
  void OnResolved(int status, AddressList * addr_list)
  {
#if TENTA_LOG_ENABLE == 1
    LOG(INFO) << "resolved: " + info_.hostname() + " status: " << status;
#endif

    if (!was_canceled())
    {
      if (status == OK && addr_list != nullptr)
        *addresses_ = AddressList::CopyWithPort(*addr_list, info_.port());

      CompletionCallback callback = callback_;
      Cancel();
      callback.Run(status);
    }
  }

  /**
   * Cancel this request
   */
  void Cancel()
  {
    callback_.Reset();
  }

  /**
   * return true if has been canceled
   */
  bool was_canceled() const
  {
    return callback_.is_null();
  }

  /**
   * Mark as being sent to java, so we expect some results in OnResolved
   */
  void sent_to_java()
  {
    sent_to_java_ = true;
  }

  /**
   * True if request was sent to java through jni
   */
  bool was_sent_to_java()
  {
    return sent_to_java_;
  }

  /**
   * Returns this requests age (time passed since created)
   */
  base::TimeDelta age()
  {
    return base::TimeTicks::Now() - when_created_;
  }

  const RequestInfo& info() const
  {
    return info_;
  }

private:
  // The request info that started the request.
  const RequestInfo info_;

  RequestPriority priority_;

  // The user's callback to invoke when the request completes.
  CompletionCallback callback_;

  // The address list to save result into.
  AddressList* addresses_;

  // Creation time (need for ageing check)
  base::TimeTicks when_created_;

  // true if request was sent to java
  bool sent_to_java_;

  OnErrorCallback on_error_call_;

};

/**********************************
 * class HostResolverTenta
 */
HostResolverTenta::HostResolverTenta(
  std::unique_ptr<HostResolver> backup_resolver)
  : weak_ptr_factory_(this),
    _use_backup(false)
{

  backup_resolver_ = std::move(backup_resolver);
  task_runner_ = base::WorkerPool::GetTaskRunner(true /* task_is_slow */);

  JNIEnv* env = AttachCurrentThread();

  j_host_resolver_ = JavaObjectWeakGlobalRef(
                       env,
                       Java_HostResolverTenta_getInstanceNative(env,
                           reinterpret_cast<intptr_t>(this))
                       .obj());

  on_error_call_ = base::Bind(&HostResolverTenta::OnError,
                              weak_ptr_factory_.GetWeakPtr());

//  orig_runner_ = base::MessageLoop::current()->task_runner();
  orig_runner_ = base::ThreadTaskRunnerHandle::Get();

#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "HostResolverTenta register "
            << NetworkChangeNotifier::ConnectionTypeToString(
              NetworkChangeNotifier::GetConnectionType());
#endif

  net::NetworkChangeNotifier::AddIPAddressObserver(this);
  net::NetworkChangeNotifier::AddConnectionTypeObserver(this);
  net::NetworkChangeNotifier::AddDNSObserver(this);

}

HostResolverTenta::~HostResolverTenta()
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "~HostResolverTenta";
#endif
  // if case we have unresolved requests
  net::NetworkChangeNotifier::RemoveIPAddressObserver(this);
  net::NetworkChangeNotifier::RemoveConnectionTypeObserver(this);
  net::NetworkChangeNotifier::RemoveDNSObserver(this);

  base::AutoLock lock(reqGuard);

  //STLDeleteValues(&requests_);
}

/**
 *
 */
int HostResolverTenta::Resolve(const RequestInfo& info,
                               RequestPriority priority,
                               AddressList* addresses,
                               const CompletionCallback& callback,
                               std::unique_ptr<Request>* out_req,
                               const NetLogWithSource& net_log)
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO)
      << "resolv name: " + info.hostname() + " using "
      + use_backup_str() + " onCon "
      + NetworkChangeNotifier::ConnectionTypeToString(
        NetworkChangeNotifier::GetConnectionType())
      + " with flags: " << info.host_resolver_flags();
#endif

  if (_use_backup)
  {
    return backup_resolver_->Resolve(info, priority, addresses, callback,
                                     out_req,
                                     net_log);
  }

  // Check that the caller supplied a valid hostname to resolve.
  std::string labeled_hostname;
  if (!DNSDomainFromDot(info.hostname(), &labeled_hostname))
    return ERR_NAME_NOT_RESOLVED;

  // TODO check requests_ age and drop the old ones

  base::TimeTicks now = base::TimeTicks::Now();
  int64_t key_id = now.ToInternalValue();

  SavedRequest * the_request = new SavedRequest(now, info, priority, callback,
      addresses);

  std::unique_ptr<SavedRequest> request(the_request);
  std::unique_ptr<Request> response(new RequestForCaller(key_id, weak_ptr_factory_.GetWeakPtr()));

  reqGuard.Acquire();
  requests_.insert(std::make_pair(key_id, std::move(request)));
  reqGuard.Release();

#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "Request ID: " << key_id;
#endif

  // post task and run
  task_runner_->PostTask(
    FROM_HERE,
    base::Bind(&HostResolverTenta::DoResolveInJava, base::Unretained(this), the_request, key_id));

  if (out_req)
    *out_req = std::move(response);  // for further interaction with base

  return ERR_IO_PENDING;
}

/**
 * Post task to Java and wait for response
 */
int HostResolverTenta::ResolveFromCache(const RequestInfo& info,
                                        AddressList* addresses,
                                        const NetLogWithSource& net_log)
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO)
      << "Resolve from cache: " + info.hostname() + " using "
      + use_backup_str() + " with flags: "
      << info.host_resolver_flags();
#endif

  if (_use_backup)
  {
    return backup_resolver_->ResolveFromCache(info, addresses, net_log);
  }

  return ResolveFromCacheWithTask(info, addresses, net_log);
//  return ResolveFromCacheDirect(info, addresses, net_log);
}

int HostResolverTenta::ResolveFromCacheDirect(const RequestInfo& info,
    AddressList* addresses,
    const NetLogWithSource& net_log)
{
  JNIEnv* env = AttachCurrentThread();

  if (!j_host_resolver_.is_uninitialized())
  {
    ScopedJavaLocalRef<jobject> gInstance = j_host_resolver_.get(env);

    ScopedJavaLocalRef<jstring> rHost;
    rHost = ConvertUTF8ToJavaString(env, info.hostname());

    ScopedJavaLocalRef<jobjectArray> jReturn =
      Java_HostResolverTenta_resolveCache(env, gInstance.obj(), rHost.obj());

    AddressList *foundAddr = ConvertIpJava2Native(env, jReturn.obj());

    if (foundAddr != nullptr)
    {
      *addresses = AddressList::CopyWithPort(*foundAddr, info.port());
      delete foundAddr;

#if TENTA_LOG_ENABLE == 1
      LOG(INFO) << "Resolved from cache: " + info.hostname() + " ipCnt: "
                << addresses->size();
#endif
      return OK;
    }
  }

#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "NoCache for: " + info.hostname();
#endif

  return ERR_DNS_CACHE_MISS;
}

/**
 *
 */
int HostResolverTenta::ResolveFromCacheWithTask(const RequestInfo& info,
    AddressList* addresses,
    const NetLogWithSource& net_log)
{
  base::WaitableEvent completion(
    base::WaitableEvent::ResetPolicy::AUTOMATIC,
    base::WaitableEvent::InitialState::NOT_SIGNALED);

  bool resolved = false;

  task_runner_->PostTask(
    FROM_HERE,
    base::Bind(&HostResolverTenta::DoResolveCacheInJava, base::Unretained(this),
               info, addresses, net_log, &completion, &resolved));

  {
    ScopedAllowWaitForLegacyWebViewApi wait;
    completion.Wait();
  }

  if (resolved)
  {
    return OK;
  }
//  return backup_resolver_->ResolveFromCache(info, addresses, net_log);
  return ERR_DNS_CACHE_MISS;
}

/**
 * Calls java to resolve the name
 */
void HostResolverTenta::DoResolveInJava(SavedRequest *request, int64_t key_id)
{
  JNIEnv* env = AttachCurrentThread();

  if (!j_host_resolver_.is_uninitialized())
  {
    ScopedJavaLocalRef<jobject> gInstance = j_host_resolver_.get(env);

    ScopedJavaLocalRef<jstring> rHost;
    rHost = ConvertUTF8ToJavaString(env, request->info().hostname());

    jint jReturn = Java_HostResolverTenta_resolve(env, gInstance.obj(),
                   rHost.obj(),
                   key_id);

    request->sent_to_java();

#if TENTA_LOG_ENABLE == 1
    LOG(INFO) << "resolv name java returned: " << jReturn;
#endif

    if (jReturn != OK)
    {
      OnError(key_id, jReturn);
    }

  }
  else
  {
    OnError(key_id, ERR_DNS_SERVER_FAILED);
  }

}

/**
 * Get cached value from java
 */
void HostResolverTenta::DoResolveCacheInJava(const RequestInfo& info,
    AddressList* addresses,
    const NetLogWithSource& net_log,
    base::WaitableEvent* completion,
    bool *success)
{
  int retVal = ResolveFromCacheDirect(info, addresses, net_log);

  if (retVal == OK)
  {
    *success = true;
  }
  else
  {
    *success = false;
  }
  completion->Signal();
}

/**
 * Prepare final AddressList and call completion callback.
 */
void HostResolverTenta::OnResolved(JNIEnv* env, jobject caller, jint status,
                                   jobjectArray result,
                                   jlong forRequestId)
{

  AddressList *foundAddr = nullptr;  // the found addresses

  if (status == OK)
  {
    foundAddr = ConvertIpJava2Native(env, result);
  }

// TODO search by id and call onresolved
  orig_runner_->PostTask(FROM_HERE,
                         base::Bind(&HostResolverTenta::origOnResolved, weak_ptr_factory_.GetWeakPtr(),
                                    forRequestId, status, base::Owned(foundAddr)));  // bind will delete the foundAddr

}

/**
 * Convert Java Ip address list to native IP's
 */
AddressList * HostResolverTenta::ConvertIpJava2Native(JNIEnv* env,
    jobjectArray jIpArray)
{

  AddressList *foundAddr = nullptr;  // the found addresses

  if (jIpArray != nullptr)
  {
    jsize len = env->GetArrayLength(jIpArray);  // array length

    if (len > 0)
    {
      foundAddr = new AddressList();

      // fill the addresses
      for (jsize i = 0; i < len; ++i)
      {
        ScopedJavaLocalRef<jbyteArray> ip_array(
          env,
          static_cast<jbyteArray>(env->GetObjectArrayElement(jIpArray, i)));

        jsize ip_bytes_len = env->GetArrayLength(ip_array.obj());
        jbyte* ip_bytes = env->GetByteArrayElements(ip_array.obj(), nullptr);

        // new IP address
        IPAddress ip(reinterpret_cast<const uint8_t*>(ip_bytes), ip_bytes_len);

        env->ReleaseByteArrayElements(ip_array.obj(), ip_bytes, JNI_ABORT);

        IPEndPoint ip_e(ip, 0);  // endpoint with port info_.port()
        foundAddr->push_back(ip_e);  // store new ip address
      }
    }
  }
  return foundAddr;
}

void HostResolverTenta::origOnResolved(int64_t forRequestId, int error,
                                       AddressList* addr_list)
{

  base::AutoLock lock(reqGuard);

  auto it = requests_.find(forRequestId);
  if (it != requests_.end())
  {
    it->second->OnResolved(error, addr_list);
//    delete it->second;
    requests_.erase(it);
  }
}
/**
 * Called when error occured (can be on any thread, will post a task to original thread
 */
void HostResolverTenta::OnError(int64_t key_id, int error)
{
  orig_runner_->PostTask(FROM_HERE,
                         base::Bind(&HostResolverTenta::origOnResolved, weak_ptr_factory_.GetWeakPtr(), key_id, error, nullptr));
}

/**
 *
 */
void HostResolverTenta::CancelRequest(int64_t key_id)
{
//
//  if (_use_backup) {
//    backup_resolver_->CancelRequest(req);
//  } else {
//
//    int64_t key_id = reinterpret_cast<int64_t>(req);

#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "CancelRequest ID: " << key_id;
#endif

  base::AutoLock lock(reqGuard);

  auto it = requests_.find(key_id);

  // cleanup the request if any
  if (it != requests_.end())
  {
    it->second->Cancel();  // mark as cancelled

    if (!it->second->was_sent_to_java())    // if not sent to java, it's safe to delete
    {
      // TODO analyse if delete is safe here!!!
//      delete it->second;
      requests_.erase(it);
    }
  }
//  }
}

/**
 * Called by system, when network ip changed
 */
void HostResolverTenta::OnIPAddressChanged()
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "OnIPAddressChanged";
#endif
}

void HostResolverTenta::OnConnectionTypeChanged(
  NetworkChangeNotifier::ConnectionType type)
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "OnConnectionTypeChanged "
            << NetworkChangeNotifier::ConnectionTypeToString(type);
#endif
}

void HostResolverTenta::OnDNSChanged()
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "OnDNSChanged";
#endif
}

void HostResolverTenta::OnInitialDNSConfigRead()
{
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "OnInitialDNSConfigRead";
#endif
}

/**
 * Get use backup as string
 */
const char* HostResolverTenta::use_backup_str()
{
  if (_use_backup)
  {
    return "native";
  }
  return "java";
}

/**
 * Register native functions
 */
bool RegisterHostResolverTentaNative(JNIEnv* env)
{
  return RegisterNativesImpl(env);
}

} /* namespace tenta */
} /* namespace xwalk */
