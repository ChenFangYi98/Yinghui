"""Private app-owned Comfy engine, with an ephemeral token and no cloud/custom nodes."""
import hmac
import os
from pathlib import Path
import runpy
import sys
from aiohttp import web

token = os.environ.pop('YINGHUI_LOCAL_TOKEN')


@web.middleware
async def authenticate(request, handler):
    if not hmac.compare_digest(request.headers.get('X-Yinghui-Token', ''), token):
        raise web.HTTPUnauthorized()
    return await handler(request)


BaseApplication = web.Application


class PrivateApplication(BaseApplication):
    def __init__(self, *args, **kwargs):
        kwargs['middlewares'] = [authenticate, *kwargs.get('middlewares', [])]
        super().__init__(*args, **kwargs)


web.Application = PrivateApplication
core = Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(core))
os.chdir(core)
runpy.run_path(str(core / 'main.py'), run_name='__main__')
