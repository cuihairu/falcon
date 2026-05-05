import sys
try:
    from PIL import Image

    # Open PNG
    img = Image.open('assets/falcon.png')

    # Create multi-size ICO
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
    ico_imgs = []
    for size in sizes:
        resized = img.resize(size, Image.Resampling.LANCZOS)
        ico_imgs.append(resized)

    # Save ICO
    ico_imgs[0].save('apps/desktop/resources/windows/falcon.ico',
                     format='ICO',
                     sizes=sizes)
    print('ICO created successfully!')
except ImportError:
    print('PIL not installed, installing...')
    import subprocess
    subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'Pillow'])
    print('Please run the script again')
