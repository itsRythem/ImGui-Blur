# Dual Pass Kawase Blur Shader for ImGui

A high-performance Gaussian approximation blur shader for ImGui using the Kawase blur algorithm with downsample passes.

## Features

- **Near-infinite blur levels** with minimal performance impact through efficient downsample passes
- **ImGui style integration** - respects `ImGuiStyleVar_Alpha` when using `ImGui::GetColorU32(ImGuiCol_X)`
- **Optimized rendering** for multiple overlapping blur regions

## Usage

### Basic Workflow

1. **Initialize**: Call `blur::setup()` to initialize the shaders and render state
2. **Garbage collect**: IMPORTANT! `blur::garbage_collect()` must be called at the beginning of the frame
3. **Update blur texture**: Call `blur::process()` to generate the blur texture
4. **Render blur**: Call `blur::render()` to draw the blurred result
5. **Access texture**: Use `blur::get_texture()` to retrieve the current blur texture
6. **Release**: Use `blur::destroy()` to free and restore any active memory or states
```cpp
// Initialize the blur
blur::setup(device, device_context);

// Update the blur texture
blur::process(draw_list, iterations, offset, noise);

// Render the blur
blur::render(draw_list, rect_min, rect_max, color, rounding, flags);

// Free the blur
blur::destroy();
```

### Performance Optimization

#### Multiple Blur Regions
When rendering multiple non-overlapping blur rectangles (e.g., simultaneous UI events):
```cpp
// ✅ GOOD: Call process once before rendering multiple regions
blur::process(draw_list, iterations, offset, noise);
blur::render(draw_list, rect_min_1, rect_max_1, color, rounding, flags);
blur::render(draw_list, rect_min_2, rect_max_2, color, rounding, flags);
blur::render(draw_list, rect_min_3, rect_max_3, color, rounding, flags);

// ❌ AVOID: Calling process multiple times
blur::process(draw_list, iterations, offset, noise);
blur::render(draw_list, rect_min_1, rect_max_2, color, rounding, flags);
blur::process(draw_list, iterations, offset);  // Expensive!
blur::render(draw_list, rect_min_2, rect_max_2, color, rounding, flags);
```

#### Performance Considerations
- **`iterations`**: Changing iteration count requires framebuffer rebuilding - **most expensive**
- **`offset`**: Relatively inexpensive to modify
- **`noise`**: (untested, should be inexpensive)

⚠️ Use `blur::process()` sparingly as it's a costly operation

## Implementation Notes

### Noise Support
A basic frosted glass / grain effect is implemented post tap for quality.

For optimization purposes, see the optimized `hash22` noise function:
- [thi-ng/umbrella noise hash implementation](https://github.com/thi-ng/umbrella/blob/38ecd7cd02564594ab21dbf0d84a44222fd7e4ef/packages/shader-ast-stdlib/src/noise/hash.ts#L113)

### Alpha Blending
To ensure proper alpha blending with ImGui's style system:
```cpp
ImU32 color = ImGui::GetColorU32(ImGuiCol_WindowBg); // Or ImGuiCol_Texture if you can modify imgui's backend!
blur::render(draw_list, rect_min, rect_max, color, rounding, flags);
```

## Algorithm

This implementation uses the **Kawase Blur** algorithm, which approximates Gaussian blur through:
1. Progressive downsampling passes
2. Dual-pass rendering (horizontal + vertical)
3. Efficient texture sampling patterns

This approach provides high-quality blur results while maintaining excellent performance characteristics.
