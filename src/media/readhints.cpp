// The thread's hints for the readers it calls (media.h, MediaReadHints).
#include "media.h"

namespace reno {

MediaReadHints& mediaReadHints()
{
    static thread_local MediaReadHints hints;
    return hints;
}

}  // namespace reno
