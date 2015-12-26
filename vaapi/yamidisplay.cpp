#include "interface/NativeDisplayHelper.h"

extern "C" {

NativeDisplay* createNativeDisplay(YamiDisplay* display)
{
    return NULL;
}

/* we hold a reference count for YamiDisplay.user, releaseNativeDisplay
 * will unref it, when the ref count equals 0, we will call YamiDisplay::free
 */
void releaseNativeDisplay(NativeDisplay* p)
{
}

} /* extern "C"*/
