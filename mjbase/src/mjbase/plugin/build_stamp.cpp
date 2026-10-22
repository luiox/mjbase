#include "build_stamp.h"

namespace mjbase::plugin {

#ifndef MJBASE_BUILD_COMMIT
#define MJBASE_BUILD_COMMIT "unknown"
#endif

const char* buildCommit() noexcept
{
    return MJBASE_BUILD_COMMIT;
}

}   // namespace mjbase::plugin
