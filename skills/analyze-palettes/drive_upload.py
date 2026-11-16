#!/usr/bin/env python3
"""Upload a finished zip to My Drive/[processed], beside [palettes].

Credentials are read from GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET and
GOOGLE_REFRESH_TOKEN. They are never written to disk or printed.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

API = "https://www.googleapis.com/drive/v3"
UPLOAD_API = "https://www.googleapis.com/upload/drive/v3"
PALETTES_ID = "1phwFvw-8xwgtFxkU1dEszvyo-UgXQhGv"
FOLDER = "application/vnd.google-apps.folder"
TARGET = "[processed]"
CREDENTIALS = ("GOOGLE_CLIENT_ID", "GOOGLE_CLIENT_SECRET", "GOOGLE_REFRESH_TOKEN")


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


def upload(token, path, folder_id, name):
    size = os.path.getsize(path)
    metadata = json.dumps({"name": name, "parents": [folder_id]}).encode()
    headers = {
        "Authorization": "Bearer " + token,
        "Content-Type": "application/json; charset=UTF-8",
        "X-Upload-Content-Type": "application/zip",
        "X-Upload-Content-Length": str(size),
    }
    with call("%s/files?uploadType=resumable&fields=id,name,size,webViewLink" % UPLOAD_API,
              metadata, headers) as response:
        session = response.headers["Location"]
    with open(path, "rb") as handle:
        request = urllib.request.Request(
            session, data=handle, method="PUT",
            headers={"Content-Type": "application/zip", "Content-Length": str(size)},
        )
        with urllib.request.urlopen(request, timeout=600) as response:
            return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", help="finished zip to upload")
    parser.add_argument("--name", help="name in Drive; defaults to the file name")
    arguments = parser.parse_args()

    if not all(os.environ.get(name) for name in CREDENTIALS):
        print("UPLOAD SKIPPED: %s not set" % " / ".join(CREDENTIALS))
        return 2
    if not os.path.isfile(arguments.zip):
        print("UPLOAD FAILED: %s is not a file" % arguments.zip)
        return 1

    try:
        token = access_token()
        folder_id = target_folder(token)
        result = upload(token, arguments.zip, folder_id, arguments.name or os.path.basename(arguments.zip))
    except UploadError as error:
        print("UPLOAD FAILED: %s" % error)
        return 1

    print("uploaded %s (%.1f MB)" % (result["name"], int(result["size"]) / 1e6))
    print(result["webViewLink"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
