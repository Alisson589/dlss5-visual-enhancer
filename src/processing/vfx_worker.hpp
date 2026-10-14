#pragma once
#include "pipeline.hpp"
namespace ve {
// Separate process ownership prevents VFX's NGX implementation from disturbing
// the working application's earlier native sessions.
[[nodiscard]] RenderResult renderIsolatedVfx(const RenderRequest&,JobControl&,const ProgressCallback&);
[[nodiscard]] QImage previewIsolatedVfx(const QString& path,const Settings&,const MediaInfo&,double seconds,JobControl&);
int runVfxWorker(const QString& path);
[[nodiscard]] QString vfxWorkerDiagnostics();
}
