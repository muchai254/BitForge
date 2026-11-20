#include "seedgen.h"
#include <Arduino.h>
#include <Bitcoin.h>
#include <esp_random.h>
#include <utility/trezor/bip39.h>
#include "bip39_words.h"
#include "display.h"
#include "entropy.h"
#include "keys.h"
#include "qr_out.h"
#include "secure_mem.h"
#include "seedqr.h"
#include "session.h"
#include "ui.h"

#define COL_DISABLED 0x4208
#define COL_WARN     0xFD20
#define MAX_DICE     300
#define WORDS_PER_PAGE 6

// Lets the user type d6 rolls into `dice` ('1'..'6'). Only the count is shown,
// never the rolls. Done is enabled from ENTROPY_MIN_DICE rolls on. Returns
// false if cancelled (dice is then cleared).
static bool diceEntry(SecretBuffer<MAX_DICE + 1> &dice) {
  static const char *labels[6] = {"1", "2", "3", "4", "5", "6"};
  Button b[9];
  for (int i = 0; i < 6; i++) {
    b[i] = {(int16_t)(6 + (i % 3) * 78), (int16_t)(96 + (i / 3) * 70), 72, 64, labels[i], 0x4208, COL_WHITE, 3};
  }
  b[6] = {8, 240, 108, 42, "Undo", COL_GREY, COL_BLACK, 2};
  b[7] = {124, 240, 108, 42, "Done", COL_DISABLED, COL_GREY, 2};
  b[8] = {8, 288, 224, 28, "Cancel", COL_BLACK, COL_GREY, 2};

  tft.fillScreen(COL_BLACK);
  drawCentredText("Dice rolls", 6, 2, COL_BTC_ORANGE);
  drawTextWrapped("Roll a die and tap the number shown. Only the count is displayed.", 8, 30, 224, 1, COL_GREY);
  for (const Button &k : b) drawButton(k, false);

  while (true) {
    char count[32];
    snprintf(count, sizeof count, "%u / %d", (unsigned)dice.size(), ENTROPY_MIN_DICE);
    tft.fillRect(0, 60, tft.width(), 26, COL_BLACK);
    drawCentredText(count, 64, 2, dice.size() >= ENTROPY_MIN_DICE ? COL_GREEN : COL_WHITE);
    bool ready = dice.size() >= ENTROPY_MIN_DICE;
    b[7].bg = ready ? COL_GREEN : COL_DISABLED;
    b[7].fg = ready ? COL_BLACK : COL_GREY;
    drawButton(b[7], false);

    int k = runButtons(b, 9);
    if (k < 6) {
      char c = '1' + k;
      dice.append(&c, 1); // ignored once MAX_DICE is reached
    } else if (k == 6) {
      if (dice.size() > 0) dice.setSize(dice.size() - 1);
    } else if (k == 7) {
      if (ready) return true;
    } else {
      dice.clear();
      return false;
    }
  }
}

static int progressPct = -1;
static void progressBar(float f) {
  int pct = (int)(f * 100);
  if (pct / 5 == progressPct / 5) return;
  progressPct = pct;
  tft.fillRect(20, 200, (tft.width() - 40) * pct / 100, 12, COL_BTC_ORANGE);
}

static void message(const char *title, const char *text, uint16_t titleColour) {
  tft.fillScreen(COL_BLACK);
  drawCentredText(title, 20, 2, titleColour);
  drawTextWrapped(text, 12, 60, tft.width() - 24, 2, COL_WHITE);
}

static void okButton() {
  const Button ok = {70, 270, 100, 40, "OK", COL_GREY, COL_BLACK, 2};
  drawButton(ok, false);
  runButtons(&ok, 1);
}

// All secret work, in its own frames (the caller scrubs the stack after).
// Returns 0 on success, or an error code.
__attribute__((noinline)) static int makeSeed(const SecretBuffer<MAX_DICE + 1> &dice) {
  uint8_t *hw = (uint8_t *)malloc(ENTROPY_HW_BYTES);
  if (!hw) return 1;
  bool hwOk = entropyCollectHardware(hw);
  if (!hwOk) {
    free(hw); // already zeroed by entropyCollectHardware
    return 2;
  }
  uint8_t ent[32];
  entropyMix(hw, ENTROPY_HW_BYTES, dice.c_str(), dice.size(), ent);
  secure_zero(hw, ENTROPY_HW_BYTES);
  free(hw);

  const char *m = mnemonicFromEntropy(ent, sizeof ent); // 32 bytes -> 24 words
  secure_zero(ent, sizeof ent);
  bool stored = m && session.mnemonic.append(m, strlen(m));
  mnemonic_clear(); // the static buffer mnemonicFromEntropy wrote into
  if (!stored || !checkMnemonic(session.mnemonic.c_str())) {
    session.mnemonic.clear();
    return 3;
  }
  return 0;
}

void generateSeedScreen() {
  if (session.loaded) {
    if (!confirmScreen("Replace seed?", "A seed is already loaded. Wipe it from memory and make a new one?")) return;
    sessionWipe();
  }

  tft.fillScreen(COL_BLACK);
  drawCentredText("New seed", 12, 2, COL_BTC_ORANGE);
  drawTextWrapped("24 words. Randomness from the hardware RNG. Optionally add 99+ dice rolls of your own.",
                  12, 44, tft.width() - 24, 1, COL_WHITE);
  drawTextWrapped("SIGNET PROTOTYPE: never use this seed for real funds.", 12, 90, tft.width() - 24, 1, COL_WARN);
  const Button choice[3] = {
      {20, 130, 200, 50, "Hardware only", COL_BTC_ORANGE, COL_BLACK, 2},
      {20, 190, 200, 50, "Add dice rolls", COL_BTC_ORANGE, COL_BLACK, 2},
      {20, 260, 200, 44, "Cancel", COL_GREY, COL_BLACK, 2},
  };
  for (const Button &b : choice) drawButton(b, false);
  int c = runButtons(choice, 3);
  if (c == 2) return;

  SecretBuffer<MAX_DICE + 1> dice;
  if (c == 1 && !diceEntry(dice)) return;
  uint16_t diceRolls = (uint16_t)dice.size();

  message("Generating", "Collecting hardware randomness. Don't touch the screen.", COL_BTC_ORANGE);
  delay(300); // let the finger come off the panel
  int err = makeSeed(dice);
  dice.clear();
  if (err) {
    secure_scrub_stack();
    Serial.printf("[seed] generation failed (code %d)\n", err);
    message("Failed", err == 2 ? "The hardware random generator failed its self-check. No seed was made."
                               : "Couldn't make a seed. Nothing was kept.",
            COL_RED);
    okButton();
    return;
  }

  message("Generating", "Deriving the fingerprint...", COL_BTC_ORANGE);
  tft.drawRect(19, 199, tft.width() - 38, 14, COL_GREY);
  progressPct = -1;
  keysFingerprint(session.mnemonic.c_str(), session.mnemonic.size(), "", 0, session.fingerprint, progressBar);
  size_t scrubbed = secure_scrub_stack();
  session.words = 24;
  session.diceRolls = diceRolls;
  session.generatedHere = true;
  session.loaded = true;

  char fp[9];
  snprintf(fp, sizeof fp, "%02x%02x%02x%02x", session.fingerprint[0], session.fingerprint[1],
           session.fingerprint[2], session.fingerprint[3]);
  // Counts and the fingerprint only: never the words, entropy or dice.
  Serial.printf("[seed] new seed: 24 words, hardware RNG %d bytes, dice rolls %u, fingerprint %s "
                "(stack scrubbed %u bytes)\n",
                ENTROPY_HW_BYTES, (unsigned)diceRolls, fp, (unsigned)scrubbed);

  tft.fillScreen(COL_BLACK);
  drawCentredText("Seed ready", 12, 2, COL_GREEN);
  char line[48];
  int y = 50;
  y = drawTextWrapped("24 words", 16, y, 210, 2, COL_WHITE) + 6;
  y = drawTextWrapped("Hardware RNG used: yes", 16, y, 210, 2, COL_WHITE) + 6;
  if (diceRolls) snprintf(line, sizeof line, "Dice rolls: %u", (unsigned)diceRolls);
  else snprintf(line, sizeof line, "Dice rolls: none");
  y = drawTextWrapped(line, 16, y, 210, 2, COL_WHITE) + 6;
  y = drawTextWrapped("Fingerprint:", 16, y, 210, 2, COL_GREY);
  drawTextWrapped(fp, 16, y, 210, 3, COL_BTC_ORANGE);
  drawTextWrapped("Next: write the words down on paper.", 16, 228, 210, 1, COL_GREY);
  const Button next = {20, 262, 200, 46, "Show words", COL_BTC_ORANGE, COL_BLACK, 2};
  drawButton(next, false);
  runButtons(&next, 1);
  showWordsScreen();
  bool passed = backupQuiz();
  Serial.printf("[seed] backup quiz %s\n", passed ? "passed" : "not completed");
}

#define QUIZ_QUESTIONS 3

// Asks which word is at 0-based position pos. Returns true if answered right.
static bool quizQuestion(int q, int pos) {
  char correct[12];
  if (!sessionWord(pos, correct, sizeof correct)) return false;
  int correctIdx = bip39Index(correct);

  // 3 distinct distractors, all different from the correct word.
  int choices[4];
  int answer = (int)(esp_random() % 4);
  for (int filled = 0; filled < 4;) {
    if (filled == answer) {
      choices[filled++] = correctIdx;
      continue;
    }
    int r = bip39RandomIndex();
    bool dup = r == correctIdx;
    for (int k = 0; k < filled; k++) dup |= choices[k] == r;
    if (!dup) choices[filled++] = r;
  }

  tft.fillScreen(COL_BLACK);
  char title[32], prompt[32];
  snprintf(title, sizeof title, "Check %d of %d", q + 1, QUIZ_QUESTIONS);
  snprintf(prompt, sizeof prompt, "Word #%d is:", pos + 1);
  drawCentredText(title, 10, 2, COL_BTC_ORANGE);
  drawCentredText(prompt, 50, 2, COL_WHITE);
  Button b[4];
  for (int i = 0; i < 4; i++) b[i] = {20, (int16_t)(90 + i * 54), 200, 46, bip39Word(choices[i]), 0x4208, COL_WHITE, 2};
  for (const Button &k : b) drawButton(k, false);
  int picked = runButtons(b, 4);
  secure_zero(correct, sizeof correct);
  bool right = picked == answer;
  secure_zero(choices, sizeof choices);
  answer = 0;
  return right;
}

bool backupQuiz() {
  while (true) {
    // 3 distinct random positions.
    int pos[QUIZ_QUESTIONS];
    for (int i = 0; i < QUIZ_QUESTIONS; i++) {
      bool dup;
      do {
        pos[i] = (int)(esp_random() % session.words);
        dup = false;
        for (int k = 0; k < i; k++) dup |= pos[k] == pos[i];
      } while (dup);
    }
    bool allRight = true;
    for (int i = 0; i < QUIZ_QUESTIONS && allRight; i++) allRight = quizQuestion(i, pos[i]);
    if (allRight) {
      session.backupConfirmed = true;
      message("Backup checked", "All 3 words were right. Keep the paper somewhere safe.", COL_GREEN);
      okButton();
      tft.fillScreen(COL_BLACK);
      return true;
    }
    tft.fillScreen(COL_BLACK);
    drawCentredText("Not right", 20, 2, COL_RED);
    drawTextWrapped("Check what you wrote down against the words on the device.", 12, 60, 216, 2, COL_WHITE);
    const Button b[2] = {
        {20, 180, 200, 46, "Show words", COL_BTC_ORANGE, COL_BLACK, 2},
        {20, 240, 200, 46, "Skip for now", COL_GREY, COL_BLACK, 2},
    };
    for (const Button &k : b) drawButton(k, false);
    if (runButtons(b, 2) == 1) {
      tft.fillScreen(COL_BLACK);
      return false;
    }
    showWordsScreen();
  }
}

// Shows the session seed as a SeedQR (compact by default, standard on
// request), after a warning. For copying onto paper and for testing.
static void showSeedQrScreen() {
  tft.fillScreen(COL_BLACK);
  drawCentredText("SeedQR", 12, 2, COL_WARN);
  drawTextWrapped("This QR IS your seed. Anyone who sees or photographs it can take the wallet.", 12, 46, 216, 2,
                  COL_WHITE);
  drawTextWrapped("Only show it to copy it onto paper, or to test with a test seed.", 12, 150, 216, 1, COL_GREY);
  const Button warn[2] = {
      {8, 262, 108, 46, "Cancel", COL_GREY, COL_BLACK, 2},
      {124, 262, 108, 46, "Show", COL_WARN, COL_BLACK, 2},
  };
  for (const Button &b : warn) drawButton(b, false);
  if (runButtons(warn, 2) == 0) return;

  SeedQrFormat fmt = SEEDQR_COMPACT;
  while (true) {
    SecretBuffer<97> qr;
    size_t n = seedqrEncode(session.mnemonic.c_str(), fmt, qr);
    tft.fillScreen(COL_BLACK);
    QrInfo q = {0, 0, 0};
    if (n) q = qrDraw(qr.data(), n, 0, 0, 240, 236, QR_ECC_LOW);
    qr.clear();
    char cap[48];
    snprintf(cap, sizeof cap, "%s v%d, %d px", fmt == SEEDQR_COMPACT ? "CompactSeedQR" : "SeedQR", q.version, q.modulePx);
    drawCentredText(q.version ? cap : "Couldn't make the SeedQR", 240, 1, q.version ? COL_GREY : COL_RED);
    drawCentredText("Never photograph a real seed", 254, 1, COL_WARN);
    const Button b[2] = {
        {8, 270, 108, 44, "Format", COL_GREY, COL_BLACK, 2},
        {124, 270, 108, 44, "Done", COL_BTC_ORANGE, COL_BLACK, 2},
    };
    for (const Button &k : b) drawButton(k, false);
    int k = runButtons(b, 2);
    if (k == 1) break;
    fmt = fmt == SEEDQR_COMPACT ? SEEDQR_STANDARD : SEEDQR_COMPACT;
  }
  secure_scrub_stack(); // the encoder's working buffers held the seed
  tft.fillScreen(COL_BLACK);
}

void showWordsScreen() {
  if (!session.loaded) {
    message("No seed", "No seed is loaded.", COL_GREY);
    okButton();
    return;
  }
  const int pages = (session.words + WORDS_PER_PAGE - 1) / WORDS_PER_PAGE;
  int page = 0;
  while (true) {
    tft.fillScreen(COL_BLACK);
    char title[48];
    int first = page * WORDS_PER_PAGE;
    int last = min(first + WORDS_PER_PAGE, (int)session.words);
    snprintf(title, sizeof title, "Words %d-%d of %d", first + 1, last, session.words);
    drawCentredText(title, 8, 2, COL_BTC_ORANGE);
    drawTextWrapped("Write them on paper. Never photograph them.", 12, 32, 216, 1, COL_WARN);
    for (int i = first; i < last; i++) {
      char word[12], line[32];
      if (!sessionWord(i, word, sizeof word)) break;
      snprintf(line, sizeof line, "%2d. %s", i + 1, word);
      tft.setTextSize(3);
      tft.setTextColor(COL_WHITE, COL_BLACK);
      tft.setCursor(16, 56 + (i - first) * 34);
      tft.print(line);
      secure_zero(word, sizeof word);
      secure_zero(line, sizeof line);
    }
    bool lastPage = page == pages - 1;
    // The last page also offers the SeedQR.
    const Button nav[3] = {
        {4, 268, 76, 46, page == 0 ? "Close" : "Back", COL_GREY, COL_BLACK, 2},
        {84, 268, 72, 46, "QR", COL_GREY, COL_BLACK, 2},
        {160, 268, 76, 46, lastPage ? "Done" : "Next", COL_BTC_ORANGE, COL_BLACK, 2},
    };
    const Button *shown = nav;
    Button two[2] = {nav[0], nav[2]};
    if (!lastPage) shown = two;
    int count = lastPage ? 3 : 2;
    for (int i = 0; i < count; i++) drawButton(shown[i], false);
    int k = runButtons(shown, count);
    bool back = k == 0, next = k == count - 1;
    if (back) {
      if (page == 0) break;
      page--;
    } else if (next) {
      if (lastPage) break;
      page++;
    } else {
      showSeedQrScreen();
    }
  }
  tft.fillScreen(COL_BLACK); // don't leave words on screen
}
