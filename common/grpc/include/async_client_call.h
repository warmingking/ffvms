#ifndef __ASYNC_CLIENT_CALL_H__
#define __ASYNC_CLIENT_CALL_H__

#include <glog/logging.h>
#include <grpc++/grpc++.h>
#include <memory>

namespace common
{
namespace grpc
{

template <typename RequestType, typename ResponseType>
using PrepareAsyncCall = std::function<
    std::unique_ptr<::grpc::ClientAsyncResponseReader<ResponseType>>(
        ::grpc::ClientContext *context, const RequestType &request,
        ::grpc::CompletionQueue *cq)>;

class AsyncClientCall
{
public:
    virtual ~AsyncClientCall(){};

    virtual void Processed(const bool ok) = 0;
};

template <typename ResponseType>
using ProcessResponseOrError =
    std::function<void(std::unique_ptr<ResponseType> &&response,
                       std::unique_ptr<::grpc::Status> &&error)>;

template <typename ResponseType> class GeneralAsyncClientCall : AsyncClientCall
{
public:
    GeneralAsyncClientCall();
    virtual ~GeneralAsyncClientCall(){};

    void SetCallback(ProcessResponseOrError<ResponseType> &&callback);

    template <typename RequestType>
    void
    StartCall(const RequestType &request, ::grpc::CompletionQueue *cq,
              PrepareAsyncCall<RequestType, ResponseType> &&prepareAsyncCall,
              const long timeoutInMs);
    void Processed(const bool ok) override;

private:
    std::unique_ptr<ResponseType> mpResponse;

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    ::grpc::ClientContext mContext;

    // Storage for the status of the RPC upon completion.
    std::unique_ptr<::grpc::Status> mpStatus;

    std::unique_ptr<::grpc::ClientAsyncResponseReader<ResponseType>>
        mpResponseReader;

    ProcessResponseOrError<ResponseType> callback;
};

template <typename ResponseType>
GeneralAsyncClientCall<ResponseType>::GeneralAsyncClientCall()
    : mpResponse(std::make_unique<ResponseType>()),
      mpStatus(std::make_unique<::grpc::Status>())
{
}

template <typename ResponseType>
void GeneralAsyncClientCall<ResponseType>::SetCallback(
    ProcessResponseOrError<ResponseType> &&callback)
{
    this->callback = std::move(callback);
}

template <typename ResponseType>
template <typename RequestType>
void GeneralAsyncClientCall<ResponseType>::StartCall(
    const RequestType &request, ::grpc::CompletionQueue *cq,
    PrepareAsyncCall<RequestType, ResponseType> &&prepareAsyncCall,
    const long timeoutInMs)
{
    if (timeoutInMs > 0)
    {
        std::chrono::system_clock::time_point deadline =
            std::chrono::system_clock::now() +
            std::chrono::milliseconds(timeoutInMs);
        mContext.set_deadline(deadline);
    }
    mpResponseReader = prepareAsyncCall(&mContext, request, cq);
    mpResponseReader->StartCall();
    mpResponseReader->Finish(mpResponse.get(), mpStatus.get(), this);
}

template <typename ResponseType>
void GeneralAsyncClientCall<ResponseType>::Processed(const bool ok)
{
    if (!ok)
    {
        mpStatus.reset(new ::grpc::Status(::grpc::StatusCode::UNKNOWN,
                                          "grpc unknown mistake"));
    }
    callback(std::move(mpResponse), std::move(mpStatus));
}

} // namespace grpc
} // namespace common

#endif // __ASYNC_CLIENT_CALL_H__