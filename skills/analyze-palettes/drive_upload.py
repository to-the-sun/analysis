#!/usr/bin/env python3
"""Upload a finished batch folder to My Drive/[processed], beside [palettes].

The batch becomes one regular folder named after --name, with its whole tree:
the master WAV, pattern_reports/ and every subfolder of it. Credentials are read
from GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET and GOOGLE_REFRESH_TOKEN. They are
never written to disk or printed.
"""

import argparse
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from concurrent.futures import ThreadPoolExecutor

API = "https://www.googleapis.com/drive/v3"
UPLOAD_API = "https://www.googleapis.com/upload/drive/v3"
PALETTES_ID = "1phwFvw-8xwgtFxkU1dEszvyo-UgXQhGv"
FOLDER = "application/vnd.google-apps.folder"
TARGET = "[processed]"
CREDENTIALS = ("GOOGLE_CLIENT_ID", "GOOGLE_CLIENT_SECRET", "GOOGLE_REFRESH_TOKEN")
RESUMABLE = 4 * 1024 * 1024
WORKERS = 6
RETRIES = 4


class UploadError(RuntimeError):
    pass


def call(url, data=None, headers=None, method=None, timeout=120):
    request = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        return urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        detail = error.read().decode()[:200]
        raise UploadError("%s %s -> %s" % (error.code, url.split("?")[0], detail)) from None


def access_token():
    values = {name: os.environ[name] for name in CREDENTIALS}
    body = urllib.parse.urlencode({
        "client_id": values["GOOGLE_CLIENT_ID"],
        "client_secret": values["GOOGLE_CLIENT_SECRET"],
        "refresh_token": values["GOOGLE_REFRESH_TOKEN"],
        "grant_type": "refresh_token",
    }).encode()
    with call("https://oauth2.googleapis.com/token", body,
              {"Content-Type": "application/x-www-form-urlencoded"}) as response:
        return json.load(response)["access_token"]


def json_call(url, token, data=None, method=None):
    headers = {"Authorization": "Bearer " + token}
    if data is not None:
        headers["Content-Type"] = "application/json"
    with call(url, data, headers, method) as response:
        return json.load(response)


def target_folder(token):
    palettes = json_call("%s/files/%s?fields=parents" % (API, PALETTES_ID), token)
    parent = (palettes.get("parents") or ["root"])[0]
    query = urllib.parse.quote(
        "'%s' in parents and name='%s' and mimeType='%s' and trashed=false" % (parent, TARGET, FOLDER)
    )
    found = json_call("%s/files?q=%s&fields=files(id)" % (API, query), token)["files"]
    if found:
        return found[0]["id"]
    metadata = json.dumps({"name": TARGET, "mimeType": FOLDER, "parents": [parent]}).encode()
    return json_call("%s/files?fields=id" % API, token, metadata, "POST")["id"]


def create_folder(token, parent, name):
    metadata = json.dumps({"name": name, "mimeType": FOLDER, "parents": [parent]}).encode()
    return json_call("%s/files?fields=id" % API, token, metadata, "POST")["id"]


def send_file(token, path, parent, name):
    size = os.path.getsize(path)
    mime = mimetypes.guess_type(name)[0] or "application/octet-stream"
    metadata = json.dumps({"name": name, "parents": [parent]}).encode()
    if size > RESUMABLE:
        headers = {
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json; charset=UTF-8",
            "X-Upload-Content-Type": mime,
            "X-Upload-Content-Length": str(size),
        }
        with call("%s/files?uploadType=resumable&fields=id" % UPLOAD_API, metadata, headers) as response:
            session = response.headers["Location"]
        with open(path, "rb") as handle:
            request = urllib.request.Request(
                session, data=handle, method="PUT",
                headers={"Content-Type": mime, "Content-Length": str(size)},
            )
            with urllib.request.urlopen(request, timeout=600) as response:
                return json.load(response)["id"]
    boundary = uuid.uuid4().hex
    with open(path, "rb") as handle:
        data = handle.read()
    body = (("--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"
             % boundary).encode() + metadata
            + ("\r\n--%s\r\nContent-Type: %s\r\n\r\n" % (boundary, mime)).encode() + data
            + ("\r\n--%s--\r\n" % boundary).encode())
    headers = {"Authorization": "Bearer " + token,
               "Content-Type": "multipart/related; boundary=" + boundary}
    with call("%s/files?uploadType=multipart&fields=id" % UPLOAD_API, body, headers) as response:
        return json.load(response)["id"]


def upload_file(token, path, parent, name):
    for attempt in range(RETRIES):
        try:
            return send_file(token, path, parent, name)
        except (UploadError, urllib.error.URLError):
            if attempt == RETRIES - 1:
                raise
            time.sleep(2 * (attempt + 1))


def tree(token, root, parent):
    folders, jobs = {"": parent}, []
    for base, _, names in os.walk(root):
        relative = os.path.relpath(base, root)
        if relative == ".":
            relative = ""
        else:
            folders[relative] = create_folder(
                token, folders[os.path.dirname(relative)], os.path.basename(relative))
        jobs += [(os.path.join(base, name), folders[relative], name) for name in sorted(names)]
    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        list(pool.map(lambda job: upload_file(token, *job), jobs))
    return jobs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", help="finished batch folder to upload")
    parser.add_argument("--name", help="folder name in Drive; defaults to the folder name")
    arguments = parser.parse_args()

    if not all(os.environ.get(name) for name in CREDENTIALS):
        print("UPLOAD SKIPPED: %s not set" % " / ".join(CREDENTIALS))
        return 2
    if not os.path.isdir(arguments.folder):
        print("UPLOAD FAILED: %s is not a folder" % arguments.folder)
        return 1

    name = arguments.name or os.path.basename(os.path.abspath(arguments.folder))
    try:
        token = access_token()
        folder_id = create_folder(token, target_folder(token), name)
        jobs = tree(token, arguments.folder, folder_id)
    except (UploadError, urllib.error.URLError) as error:
        print("UPLOAD FAILED: %s" % error)
        return 1

    print("uploaded %d files (%.1f MB) into %s" % (
        len(jobs), sum(os.path.getsize(job[0]) for job in jobs) / 1e6, name))
    print("https://drive.google.com/drive/folders/%s" % folder_id)
    return 0


if __name__ == "__main__":
    sys.exit(main())
