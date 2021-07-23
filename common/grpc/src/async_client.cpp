#include "async_client.h"

using namespace common::grpc;
using namespace grpc;

AsyncClient::~AsyncClient()
{
    for (int i = 0; i < mThreadNum; i++)
    {
        mpCqs[i]->Shutdown();
        mThreads[i].join();
    }
}

AsyncClient::AsyncClient(int threadNum) : mThreadNum(threadNum)
{
    for (int i = 0; i < threadNum; i++)
    {
        mpCqs.emplace_back(std::make_unique<CompletionQueue>());
    }
    for (int i = 0; i < threadNum; i++)
    {
        mThreads.emplace_back(std::thread(
            [this](int idx) {
                void *got_tag;
                bool ok = false;

                // Block until the next result is available in the completion
                // queue "cq".
                while (mpCqs[idx]->Next(&got_tag, &ok))
                {
                    AsyncClientCall *call =
                        static_cast<AsyncClientCall *>(got_tag);

                    call->Processed(ok);
                    delete call;
                }
            },
            i));
    }
}
