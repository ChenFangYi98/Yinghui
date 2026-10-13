"""Render the Chinese release guides as readable, self-contained HTML files."""
from html import escape
from pathlib import Path

try:
    from markdown_it import MarkdownIt
except ImportError as exc:
    raise SystemExit('Install the documentation renderer: pip install markdown-it-py') from exc


PUBLICATION = Path(__file__).resolve().parents[1] / 'publication'
DOCUMENTS = ('使用手册.zh-CN.md', '功能说明.zh-CN.md')
STYLE = """
@media (prefers-color-scheme: dark) { :root { color-scheme: dark; } }
body { margin: 0; font: 16px/1.8 -apple-system, BlinkMacSystemFont, 'Microsoft YaHei',
       'PingFang SC', 'Noto Sans CJK SC', sans-serif; background: #f5f7fb; color: #1e293b; }
main { max-width: 920px; margin: 3rem auto; padding: 3rem 4rem; background: white;
       box-shadow: 0 12px 36px #17264b16; border-radius: 18px; }
h1, h2, h3 { line-height: 1.35; color: #10203a; }
h1 { font-size: 2.2rem; border-bottom: 3px solid #3e79e8; padding-bottom: .6rem; }
h2 { margin-top: 2.5rem; border-left: 4px solid #3e79e8; padding-left: .8rem; }
a { color: #205cbb; } code { background: #eef2f8; padding: .1em .3em; border-radius: 4px; }
pre { overflow-x: auto; padding: 1rem; background: #eef2f8; border-radius: 8px; }
pre code { padding: 0; } blockquote { border-left: 4px solid #b4c6e5; margin: 1rem 0;
    padding: .15rem 1rem; background: #f3f6fb; }
table { width: 100%; border-collapse: collapse; display: block; overflow-x: auto; }
th, td { border: 1px solid #dce4ef; padding: .55rem .7rem; text-align: left; vertical-align: top; }
th { background: #eaf1fc; } li { margin: .35rem 0; }
.top { color: #56647a; font-size: .93rem; }
@media (max-width: 720px) { main { margin: 0; padding: 1.25rem; border-radius: 0; } }
@media (prefers-color-scheme: dark) {
  body { background: #121925; color: #dce7f7; }
  main { background: #1e293b; } h1, h2, h3 { color: #f0f5fc; }
  a { color: #91baff; } code, pre, blockquote { background: #29384d; }
  th { background: #30445e; } th, td { border-color: #46566a; }
}
"""


def main():
    renderer = MarkdownIt('default', {'html': False})
    for name in DOCUMENTS:
        source = PUBLICATION / name
        title = source.stem
        body = renderer.render(source.read_text(encoding='utf-8'))
        page = ('<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">'
                '<meta name="viewport" content="width=device-width, initial-scale=1">'
                f'<title>{escape(title)}</title><style>{STYLE}</style></head><body>'
                f'<main><p class="top">映绘 Yinghui 0.4.5 · 离线中文文档</p>{body}</main>'
                '</body></html>')
        target = source.with_suffix('.html')
        target.write_text(page, encoding='utf-8')
        print(target.name, target.stat().st_size)


if __name__ == '__main__':
    main()
