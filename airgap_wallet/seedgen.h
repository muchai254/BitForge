// New-seed generation and word display.
#pragma once

// Generates a new 24-word seed from the hardware RNG (plus optional dice
// rolls), stores it in the session and shows it. Runs until done or cancelled.
void generateSeedScreen();

// Shows the session's words, 6 per screen, numbered.
void showWordsScreen();

// Asks for 3 random word positions (multiple choice). On a wrong answer the
// user can look at the words again and retry. Returns true once passed
// (session.backupConfirmed is then set), false if the user gives up.
bool backupQuiz();
