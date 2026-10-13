"""Render Yinghui's original layered-image mark into native Windows icon sizes."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1] / 'assets'
SVG = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
<defs><linearGradient id="a" x2="1" y2="1"><stop stop-color="#55e4e6"/><stop offset="1" stop-color="#5d8dff"/></linearGradient></defs>
<rect x="8" y="8" width="240" height="240" rx="54" fill="#172438"/>
<path d="M43 150 128 104 213 150 128 202Z" fill="#8259e8"/>
<path d="M43 123 128 77 213 123 128 175Z" fill="url(#a)"/>
<path d="m85 110 43 28 43-28-10 33-23 16v25h-20v-25l-23-16Z" fill="#fff"/>
<path d="m197 39 7 18 18 7-18 7-7 18-7-18-18-7 18-7Z" fill="#a5f6f1"/>
</svg>'''


def icon(size):
    scale = 4
    canvas = Image.new('RGBA', (256 * scale, 256 * scale))
    draw = ImageDraw.Draw(canvas)
    def polygon(points, fill):
        draw.polygon([(x * scale, y * scale) for x, y in points], fill=fill)
    draw.rounded_rectangle((8*scale, 8*scale, 248*scale, 248*scale), radius=54*scale, fill='#172438')
    polygon([(43,150),(128,104),(213,150),(128,202)], '#8259e8')
    mask = Image.new('L', canvas.size)
    ImageDraw.Draw(mask).polygon([(x*scale,y*scale) for x,y in [(43,123),(128,77),(213,123),(128,175)]], fill=255)
    gradient = Image.new('RGBA', canvas.size)
    gd = ImageDraw.Draw(gradient)
    for y in range(canvas.height):
        t = max(0, min(1, (y / scale - 77) / 98))
        color = tuple(round(a+(b-a)*t) for a,b in zip((85,228,230),(93,141,255))) + (255,)
        gd.line((0,y,canvas.width,y), fill=color)
    canvas.paste(gradient,(0,0),mask)
    polygon([(85,110),(128,138),(171,110),(161,143),(138,159),(138,184),(118,184),(118,159),(95,143)], '#ffffff')
    polygon([(197,39),(204,57),(222,64),(204,71),(197,89),(190,71),(172,64),(190,57)], '#a5f6f1')
    return canvas.resize((size,size),Image.Resampling.LANCZOS)


if __name__ == '__main__':
    ROOT.mkdir(exist_ok=True)
    (ROOT / 'yinghui.svg').write_text(SVG,encoding='utf-8')
    icon(256).save(ROOT / 'yinghui.png')
    icon(256).save(ROOT / 'yinghui.ico',sizes=[(n,n) for n in (16,24,32,48,64,128,256)])
