from PIL import Image
import struct

# Paste your LVGL C array here as a bytearray (e.g., from const uint8_t[] = {0xFF, 0x00, ...})
pixel_data = bytearray([

])

# From lv_img_dsc_t.header
width = 100  # Replace with your image width
height = 100  # Replace with your image height
color_format = 'RGB565'  # Replace if different (e.g., 'ARGB8888' for 32-bit)

# Create image based on format
if color_format == 'RGB565A8':
    img = Image.new('RGBA', (width, height))  # Use RGBA mode for alpha
else:
    img = Image.new('RGB', (width, height))

# Interpret pixels based on format
if color_format == 'RGB565':
    for y in range(height):
        for x in range(width):
            idx = (y * width + x) * 2  # 2 bytes per pixel
            if idx + 1 < len(pixel_data):
                rgb565 = struct.unpack('<H', pixel_data[idx:idx+2])[0]
                r = ((rgb565 >> 11) & 0x1F) << 3
                g = ((rgb565 >> 5) & 0x3F) << 2
                b = (rgb565 & 0x1F) << 3
                img.putpixel((x, y), (r, g, b))

elif color_format == 'RGB565A8':
    color_size = width * height * 2  # RGB565 part
    alpha_size = width * height  # 8-bit alpha part
    if len(pixel_data) < color_size + alpha_size:
        print("Invalid data size for RGB565A8")
    else:
        colors = pixel_data[0:color_size]
        alphas = pixel_data[color_size:color_size + alpha_size]
        for y in range(height):
            for x in range(width):
                c_idx = (y * width + x) * 2
                a_idx = y * width + x
                if c_idx + 1 < len(colors) and a_idx < len(alphas):
                    rgb565 = struct.unpack('<H', colors[c_idx:c_idx+2])[0]
                    a = alphas[a_idx]
                    r = ((rgb565 >> 11) & 0x1F) << 3
                    g = ((rgb565 >> 5) & 0x3F) << 2
                    b = (rgb565 & 0x1F) << 3
                    img.putpixel((x, y), (r, g, b, a))

# Save as PNG
img.save('output_image.png')
print("Image saved as output_image.png")