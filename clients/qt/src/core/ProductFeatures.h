#pragma once

// Public standalone targets default to the community feature set.
#ifndef GZYD_FEATURE_ERP
#define GZYD_FEATURE_ERP 0
#endif
#ifndef GZYD_FEATURE_INTERNAL_HUB
#define GZYD_FEATURE_INTERNAL_HUB 0
#endif

namespace ProductFeatures {
inline constexpr bool erp = GZYD_FEATURE_ERP != 0;
inline constexpr bool internalHub = GZYD_FEATURE_INTERNAL_HUB != 0;
}
