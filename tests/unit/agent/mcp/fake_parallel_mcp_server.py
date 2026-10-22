#!/usr/bin/env python3
"""Deterministic Parallel-shaped MCP fixture; never contacts Parallel."""
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with open(os.environ['PARALLEL_TEST_LOG'], 'a', encoding='utf-8') as log:
            log.write(json.dumps({'path': self.path, 'ua': self.headers.get('User-Agent'),
                                  'authorization': self.headers.get('Authorization'),
                                  'request': req}) + '\n')
        method = req['method']
        params = req.get('params', {})
        response = {'jsonrpc': '2.0', 'id': req.get('id')}
        if method == 'server/discover':
            response['error'] = {'code': -32601, 'message': 'Method not found'}
        elif method == 'initialize':
            response['result'] = {'protocolVersion': '2025-11-25', 'capabilities': {},
                                  'serverInfo': {'name': 'parallel-fixture', 'version': '1'}}
        elif method == 'tools/list':
            response['result'] = {'tools': [
                {'name': 'web_search', 'description': 'Search the web',
                 'inputSchema': {'type': 'object', 'properties': {
                     'objective': {'type': 'string'}, 'search_queries': {'type': 'array'}},
                     'required': ['objective', 'search_queries']}},
                {'name': 'web_fetch', 'description': 'Fetch pages',
                 'inputSchema': {'type': 'object', 'properties': {'urls': {'type': 'array'}},
                                 'required': ['urls']}}]}
        elif method == 'tools/call':
            args = params['arguments']
            if args.get('objective') == 'slow':
                time.sleep(0.3)
            if args.get('objective') == 'rpc-error':
                response['error'] = {'code': -32602, 'message': 'Invalid search arguments'}
            else:
                failed = args.get('objective') == 'tool-error'
                text = ('Search quota exceeded' if failed else
                        ('CMake FetchContent https://cmake.org/cmake/help/latest/module/FetchContent.html'
                         if params['name'] == 'web_search' else '# FetchContent\nPopulate content at configure time.'))
                response['result'] = {'content': [{'type': 'text', 'text': text}], 'isError': failed}
        else:
            self.send_response(202)
            self.end_headers()
            return
        payload = json.dumps(response).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except BrokenPipeError:
            pass  # timeout test deliberately closes its connection

    def log_message(self, *_args):
        pass


if __name__ == '__main__':
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    print('PORT=' + str(server.server_port), flush=True)
    server.serve_forever()
