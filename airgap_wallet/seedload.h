// Loading an existing seed by typing its words.
#pragma once

// Asks for 12 or 24 words (with BIP39 autocomplete), checks the checksum,
// asks for an optional passphrase, and loads the seed into the session.
// Always shows the resulting fingerprint.
void loadSeedScreen();
