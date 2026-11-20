#include "DebuggerTypes.h"

int decodeDbmsDebugOer(int oer) {
    if (oer == 1403) return 100;
    if (oer == 6510) return 1;
    return oer == 0 ? 0 : -qAbs(oer);
}
