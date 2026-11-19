#pragma once

// Append one line to sd:/diag.txt (opened, written, closed and flushed per call,
// so the log survives a hang / power cut).
void diag_log(const char* format, ...);
