// Watch-only export to Sparrow, and the QR loopback test.
#pragma once

// Shows the session's BIP84 signet account as a QR for Sparrow, in two
// switchable text formats (output descriptor, key with origin), plus the
// first receive/change addresses for comparison.
void exportScreen();

// Dev test: encoder capacity self-test, then shows a test QR and reports
// whether the camera reads it back unchanged over the camera link.
void qrLoopbackTestScreen();
