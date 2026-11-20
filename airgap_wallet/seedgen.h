#pragma once

// Generates a new 24-word seed from the hardware RNG (plus optional dice
// rolls), stores it in the session and shows it.
void generateSeedScreen();

void showWordsScreen();

// Asks for 3 random word positions (multiple choice). On a wrong answer the
// user can look at the words again and retry. Returns true once passed
// (session.backupConfirmed is then set), false if the user gives up.
bool backupQuiz();
