#include "seedload.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include "bip39_words.h"
#include "camlink.h"
#include "display.h"
#include "keys.h"
#include "secure_mem.h"
#include "seedqr.h"
#include "session.h"
#include "touch.h"
#include "ui.h"

#define MAX_WORDS 24
#define WORD_CAP 10 // longest BIP39 word is 8 letters

// Keyboard: 7 x 4 keys (a..z, DEL, BACK), same geometry as ui.cpp's.
#define KB_COLS 7
#define KB_TOP 140
#define KB_KEY_W 34
#define KB_KEY_H 44
#define KB_LEFT ((240 - KB_COLS * KB_KEY_W) / 2)
#define KEY_DEL 26
#define KEY_BACK 27
#define MAX_CANDIDATES 4

#define COL_KEY 0x4208
#define COL_KEY_FN 0xA514
#define COL_FIELD 0x10A2
#define COL_CAND 0x0560

static void drawWordField(const char *prefix) {
  tft.fillRoundRect(8, 30, 224, 30, 4, COL_FIELD);
  tft.setTextSize(3);
  tft.setTextColor(COL_WHITE);
  tft.setCursor(16, 34);
  tft.print(prefix);
  tft.print('_');
}

// Fills cand[] with buttons for the words matching prefix: all of them if
// there are at most 4, otherwise the first 3 (the 4th slot then says how many
// more there are). Returns how many words match in total.
static int buildCandidates(const char *prefix, Button cand[MAX_CANDIDATES], int *shown) {
  *shown = 0;
  if (!prefix[0]) return 0;
  int first;
  int n = bip39PrefixRange(prefix, &first);
  int limit = n > MAX_CANDIDATES ? MAX_CANDIDATES - 1 : n;
  for (int i = 0; i < limit; i++) {
    cand[i] = {(int16_t)(8 + (i % 2) * 114), (int16_t)(64 + (i / 2) * 37), 110, 34, bip39Word(first + i),
               COL_CAND, COL_WHITE, 2};
    (*shown)++;
  }
  return n;
}

static void drawCandidates(const Button cand[], int shown, int total) {
  tft.fillRect(0, 62, tft.width(), 76, COL_BLACK);
  for (int i = 0; i < shown; i++) drawButton(cand[i], false);
  if (total > shown) { // the 4th slot (bottom right)
    char more[24];
    snprintf(more, sizeof more, "+%d more", total - shown);
    tft.setTextSize(2);
    tft.setTextColor(COL_GREY);
    tft.setCursor(130, 64 + 37 + 10);
    tft.print(more);
  }
}

// One word. `word` (WORD_CAP) holds the current value (may be prefilled) and
// receives the chosen word. Returns 1 when a word was chosen, -1 for BACK.
static int wordEntry(int n, int total, char *word) {
  static char letters[26][2];
  Button keys[28 + MAX_CANDIDATES];
  for (int i = 0; i < 26; i++) {
    letters[i][0] = 'a' + i;
    letters[i][1] = 0;
  }
  for (int i = 0; i < 28; i++) {
    int16_t x = KB_LEFT + (i % KB_COLS) * KB_KEY_W, y = KB_TOP + (i / KB_COLS) * KB_KEY_H;
    if (i < 26) keys[i] = {x, y, KB_KEY_W, KB_KEY_H, letters[i], COL_KEY, COL_WHITE, 2};
    else if (i == KEY_DEL) keys[i] = {x, y, KB_KEY_W, KB_KEY_H, "DEL", COL_KEY_FN, COL_BLACK, 1};
    else keys[i] = {x, y, KB_KEY_W, KB_KEY_H, "BACK", COL_KEY_FN, COL_BLACK, 1};
  }

  char prefix[WORD_CAP];
  strncpy(prefix, word, WORD_CAP - 1);
  prefix[WORD_CAP - 1] = 0;

  tft.fillScreen(COL_BLACK);
  char title[24];
  snprintf(title, sizeof title, "Word %d of %d", n + 1, total);
  drawCentredText(title, 6, 2, COL_BTC_ORANGE);
  for (int i = 0; i < 28; i++) drawButton(keys[i], false);

  int result = 0;
  while (result == 0) {
    drawWordField(prefix);
    int shown;
    int matches = buildCandidates(prefix, keys + 28, &shown);
    drawCandidates(keys + 28, shown, matches);

    int k = runButtons(keys, 28 + shown);
    size_t len = strlen(prefix);
    if (k < 26) {
      if (len < 8) {
        // Only accept letters that still match some word.
        prefix[len] = 'a' + k;
        prefix[len + 1] = 0;
        if (bip39PrefixRange(prefix, nullptr) == 0) prefix[len] = 0;
      }
    } else if (k == KEY_DEL) {
      if (len) prefix[len - 1] = 0;
    } else if (k == KEY_BACK) {
      result = -1;
    } else {
      strncpy(word, keys[k].label, WORD_CAP - 1);
      word[WORD_CAP - 1] = 0;
      result = 1;
    }
  }
  secure_zero(prefix, sizeof prefix);
  return result;
}

// --- Flow ---

static int progressPct = -1;
static void progressBar(float f) {
  int pct = (int)(f * 100);
  if (pct / 5 == progressPct / 5) return;
  progressPct = pct;
  tft.fillRect(20, 200, (tft.width() - 40) * pct / 100, 12, COL_BTC_ORANGE);
}

static int choose(const char *title, const char *text, const Button *b, int n) {
  tft.fillScreen(COL_BLACK);
  drawCentredText(title, 12, 2, COL_BTC_ORANGE);
  if (text) drawTextWrapped(text, 12, 44, 216, 1, COL_WHITE);
  for (int i = 0; i < n; i++) drawButton(b[i], false);
  return runButtons(b, n);
}

// Types `total` words into m (checksum verified). Returns false if cancelled.
static bool typeWords(SecretBuffer<SESSION_MNEMONIC_CAP> &m, int total) {
  int16_t idx[MAX_WORDS];
  for (int i = 0; i < MAX_WORDS; i++) idx[i] = -1;
  char cur[WORD_CAP];
  bool loaded = false, cancelled = false;

  while (!loaded && !cancelled) {
    // Enter (or review) every word.
    int i = 0;
    while (i < total && !cancelled) {
      if (idx[i] >= 0) strncpy(cur, bip39Word(idx[i]), WORD_CAP);
      else cur[0] = 0;
      cur[WORD_CAP - 1] = 0;
      int r = wordEntry(i, total, cur);
      if (r > 0) {
        idx[i] = (int16_t)bip39Index(cur);
        i++;
      } else if (i > 0) {
        i--;
      } else if (confirmScreen("Cancel?", "Stop loading? Nothing typed so far is kept.")) {
        cancelled = true;
      }
      secure_zero(cur, sizeof cur);
    }
    if (cancelled) break;

    m.clear();
    bool fits = true;
    for (int w = 0; w < total && fits; w++) {
      const char *word = bip39Word(idx[w]);
      fits = m.append(word, strlen(word)) && (w == total - 1 || m.append(" ", 1));
    }
    if (fits && checkMnemonic(m.c_str())) {
      loaded = true;
      break;
    }
    const Button bad[3] = {
        {20, 130, 200, 46, "Review words", COL_BTC_ORANGE, COL_BLACK, 2},
        {20, 186, 200, 46, "Start over", COL_GREY, COL_BLACK, 2},
        {20, 256, 200, 44, "Cancel", COL_GREY, COL_BLACK, 2},
    };
    int b = choose("Checksum wrong", "These words aren't a valid seed: at least one word is wrong. "
                   "Review them (they stay filled in) or start over.", bad, 3);
    if (b == 1) {
      for (int w = 0; w < MAX_WORDS; w++) idx[w] = -1;
    } else if (b == 2) {
      cancelled = true;
    }
  }
  secure_zero(idx, sizeof idx);
  if (cancelled) m.clear();
  return !cancelled;
}

// Waits for a SeedQR on the camera link and decodes it into m. Payloads are
// never shown or logged (only counted). Returns false if the user goes back.
static bool scanSeedQr(SecretBuffer<SESSION_MNEMONIC_CAP> &m, int *words, SeedQrFormat *fmt) {
  tft.fillScreen(COL_BLACK);
  drawCentredText("Scan SeedQR", 12, 2, COL_BTC_ORANGE);
  drawTextWrapped("Point the camera at the SeedQR (Standard or Compact), 15-25 cm away, and hold it steady.",
                  12, 48, 216, 2, COL_WHITE);
  drawTextWrapped("The camera module sees the seed while it scans. Only scan test seeds while the dev "
                  "viewer is running.",
                  12, 170, 216, 1, COL_GREY);
  const Button back = {150, 282, 84, 34, "Back", COL_GREY, COL_BLACK, 2};
  drawButton(back, false);

  camlinkReset();
  int rejected = 0;
  bool backHeld = false;
  while (true) {
    if (camlinkPoll()) {
      *words = seedqrDecode(camlinkPayload(), camlinkPayloadLen(), m, fmt);
      if (*words) {
        size_t zeroed = camlinkWipe();
        Serial.printf("[seed] SeedQR scanned (%s, %d words); camera link buffers wiped, %u bytes of free heap zeroed\n",
                      *fmt == SEEDQR_COMPACT ? "compact" : "standard", *words, (unsigned)zeroed);
        return true;
      }
      rejected++;
      Serial.printf("[seed] scanned QR is not a valid SeedQR (%u bytes); %d rejected\n",
                    (unsigned)camlinkPayloadLen(), rejected);
      char line[40];
      snprintf(line, sizeof line, "Not a valid SeedQR (%d)", rejected);
      tft.fillRect(0, 250, 146, 24, COL_BLACK);
      tft.setTextSize(1);
      tft.setTextColor(COL_RED);
      tft.setCursor(8, 258);
      tft.print(line);
    }
    TouchEvent e = touchPoll();
    if (e.type == TOUCH_PRESS && buttonHit(back, e.x, e.y)) {
      backHeld = true;
      drawButton(back, true);
    } else if (e.type == TOUCH_RELEASE && backHeld) {
      camlinkWipe(); // whatever came in may still be seed material
      m.clear();
      return false;
    }
    delay(5);
  }
}

// Passphrase, fingerprint and summary: shared by typing and scanning.
static void finishLoad(SecretBuffer<SESSION_MNEMONIC_CAP> &m, int total, const char *how) {
  const Button passB[2] = {
      {20, 150, 200, 50, "No passphrase", COL_BTC_ORANGE, COL_BLACK, 2},
      {20, 210, 200, 50, "Passphrase", COL_GREY, COL_BLACK, 2},
  };
  int p = choose("Passphrase?", "A BIP39 passphrase makes a different wallet from the same words. "
                 "Any letters, digits, symbols and spaces can be typed.", passB, 2);
  session.passphrase.clear();
  if (p == 1) {
    size_t n = keyboardInput("Passphrase", session.passphrase.c_str(), SESSION_PASSPHRASE_CAP);
    session.passphrase.setSize(n);
  }
  session.mnemonic.clear();
  session.mnemonic.append(m.c_str(), m.size());
  m.clear();

  tft.fillScreen(COL_BLACK);
  drawCentredText("Loading", 20, 2, COL_BTC_ORANGE);
  drawCentredText("Deriving the fingerprint...", 150, 1, COL_WHITE);
  tft.drawRect(19, 199, tft.width() - 38, 14, COL_GREY);
  progressPct = -1;
  keysFingerprint(session.mnemonic.c_str(), session.mnemonic.size(), session.passphrase.c_str(),
                  session.passphrase.size(), session.fingerprint, progressBar);
  size_t scrubbed = secure_scrub_stack();
  session.words = (uint8_t)total;
  session.diceRolls = 0;
  session.generatedHere = false;
  session.backupConfirmed = false;
  session.loaded = true;

  char fp[9];
  snprintf(fp, sizeof fp, "%02x%02x%02x%02x", session.fingerprint[0], session.fingerprint[1],
           session.fingerprint[2], session.fingerprint[3]);
  // Counts and the fingerprint only.
  Serial.printf("[seed] loaded (%s): %d words, passphrase %s, fingerprint %s (stack scrubbed %u bytes)\n", how,
                total, session.passphrase.size() ? "yes" : "no", fp, (unsigned)scrubbed);

  tft.fillScreen(COL_BLACK);
  drawCentredText("Seed loaded", 12, 2, COL_GREEN);
  char line[40];
  snprintf(line, sizeof line, "%d words, passphrase: %s", total, session.passphrase.size() ? "yes" : "no");
  drawTextWrapped(line, 16, 56, 210, 2, COL_WHITE);
  drawTextWrapped("Fingerprint:", 16, 110, 210, 2, COL_GREY);
  drawTextWrapped(fp, 16, 134, 210, 3, COL_BTC_ORANGE);
  drawTextWrapped("Check it matches the fingerprint you noted when you made this seed. "
                  "If not, a word or the passphrase is different.",
                  16, 176, 210, 1, COL_WHITE);
  const Button ok = {70, 262, 100, 46, "OK", COL_GREY, COL_BLACK, 2};
  drawButton(ok, false);
  runButtons(&ok, 1);
}

void loadSeedScreen() {
  if (session.loaded) {
    if (!confirmScreen("Replace seed?", "A seed is already loaded. Wipe it from memory and load another?")) return;
    sessionWipe();
    secure_scrub_stack();
  }

  const Button how[4] = {
      {20, 90, 200, 46, "Type 12 words", COL_BTC_ORANGE, COL_BLACK, 2},
      {20, 144, 200, 46, "Type 24 words", COL_BTC_ORANGE, COL_BLACK, 2},
      {20, 198, 200, 46, "Scan SeedQR", COL_BTC_ORANGE, COL_BLACK, 2},
      {20, 266, 200, 44, "Cancel", COL_GREY, COL_BLACK, 2},
  };
  int s = choose("Load seed", "Type the words of your backup (suggestions appear as you type), or scan a SeedQR.",
                 how, 4);
  if (s == 3) return;

  SecretBuffer<SESSION_MNEMONIC_CAP> m;
  if (s == 2) {
    int words = 0;
    SeedQrFormat fmt = SEEDQR_NONE;
    bool ok = scanSeedQr(m, &words, &fmt);
    secure_scrub_stack();
    if (!ok) return;
    finishLoad(m, words, fmt == SEEDQR_COMPACT ? "CompactSeedQR" : "SeedQR");
  } else {
    int total = s == 0 ? 12 : 24;
    bool ok = typeWords(m, total);
    secure_scrub_stack();
    if (!ok) return;
    finishLoad(m, total, "typed");
  }
}
