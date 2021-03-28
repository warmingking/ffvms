#ifndef __ASYNC_CLIENT_H__
#define __ASYNC_CLIENT_H__

#include "async_client_call.h"
#include <functional>
#include <grpc++/grpc++.h>
#include <memory>
#include <thread>
#include <vector>

namespace common
{
namespace grpc
{

class AsyncClient
{
public:
    AsyncClient(int threadNum);
    ~AsyncClient();

    template <typename RequestType, typename ResponseType>
    void Call(PrepareAsyncCall<RequestType, ResponseType> &&prepareAsyncCall,
              const RequestType &request, const long timeoutInMs,
              ProcessResponseOrError<ResponseType> &&callback);

private:
    std::atomic_uint64_t mCursor;
    std::vector<std::thread> mThreads;
    std::vector<std::unique_ptr<::grpc::CompletionQueue>> mpCqs;
    int mThreadNum;
};

/**
 * @brief rpc call
 * 
 * @tparam RequestType
 * @tparam ResponseType
 * @param prepareAsyncCall
 * @param request
 * @param timeoutInMs make sense only if > 0, otherwise timeout will not be set
 * @param callback
 */
template <typename RequestType, typename ResponseType>
void AsyncClient::Call(
    PrepareAsyncCall<RequestType, ResponseType> &&prepareAsyncCall,
    const RequestType &request, const long timeoutInMs,
    ProcessResponseOrError<ResponseType> &&callback)
{
    GeneralAsyncClientCall<ResponseType> *call =
        new GeneralAsyncClientCall<ResponseType>();
    call->SetCallback(std::move(callback));
    call->StartCall(request, mpCqs[mCursor.fetch_add(1) % mThreadNum].get(),
                    std::move(prepareAsyncCall), timeoutInMs);
}

} // namespace grpc
} // namespace common

#endif // __ASYNC_CLIENT_H__