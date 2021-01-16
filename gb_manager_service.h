#ifndef GB_MANAGER_SERVICE_H
#define GB_MANAGER_SERVICE_H

#include <functional>
#include "common.h"

using InviteCompleteCallback = std::function<void(const VideoRequest& request, const int error)>;

class GbManagerService {
public:
    void inviteAsync(const VideoRequest& request, const InviteCompleteCallback& callback);
};

#endif
