#pragma once

// Test tool (setting probe_image_quality): sets the engine's own render scale (via.render.RenderConfig.ImageQualityRate)
// and logs the main scene's layer tree with the size of every render target, to see which passes run at the render
// size and which at the output size. Off (0) by default.
namespace pipeline_probe {
void configure(int image_quality_percent);
void on_begin_rendering();              // game thread: applies the render scale
void on_prepare_output_draw(void* scene_layer); // render thread: dumps the tree when asked

// Logs the type database methods (of via.* types) whose names contain one of `words`.
void find_methods(const char* const* words, size_t count);

// Logs the code of a type database method (and of the first function it calls), to read which field it uses.
void dump_code(const char* type, const char* method);
} // namespace pipeline_probe
