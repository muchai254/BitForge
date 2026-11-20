// The transaction review on the device's own screen, the only display that
// can be trusted: one screen per recipient, then a summary. Reject is always
// the first button; Approve needs a deliberate tap plus a confirmation (and
// another one for a high fee).
#pragma once
#include "psbt_review.h"

// Shows why a PSBT was refused, until OK.
void psbtRefusedScreen(const char *reason);

// Returns true only if the user approved everything.
bool psbtReviewScreens(const Review &r);
