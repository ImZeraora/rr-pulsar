#ifndef _PUL_SCOREBASED_RESULTS_
#define _PUL_SCOREBASED_RESULTS_
#include <MarioKartWii/UI/Page/Page.hpp>
namespace Pulsar { namespace ScoreBased {
// Returns null for ordinary battle; the caller uses the native page factory.
Page *CreateBossResultsPage(PageId id);
} }
#endif
