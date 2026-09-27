#pragma once

#include <string>

#include "core/trace_recorder.h"
#include "ui/file_dialog.h"

struct App;

// Measurement > Recording Options (old RecordingDialog, "Record Trace"): edits a copy of
// App::recorder.config and shows the recorder status live. Opens on Command::RecordingOptions.
struct RecordingDialogState
{
    RecordingConfig edit;
    bool split = false;
    int split_mb = 100;
    FileDialog folder_dialog; // "Browse..."
};

void draw_recording_dialog(App& app, RecordingDialogState& s);
