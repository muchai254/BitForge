#pragma once

// Dev tests -> "PSBT self-test": runs every fixture of
// tools/psbt_fixtures/make_fixtures.js (honest and tampered PSBTs) through
// the real parser and verifier, against the signet test wallet's public
// account key, and compares verdict, amounts, fee and addresses. Needs no seed.
// Logs each case to Serial, shows PASS/FAIL, waits for Back.
void psbtSelfTestScreen();
