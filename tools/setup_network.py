#!/usr/bin/env python3
"""Write PicoPage/network.toml and PicoPage/sync.toml on a mounted Pico Page TF card.

Values are checked against the limits the firmware parsers enforce before
anything is written. The KOSync password is never stored or printed; only its
MD5, which KOReader calls the userkey, reaches the card.

Usage: setup_network.py [--offline] [CARD_ROOT]
"""
import argparse
import getpass
import glob
import hashlib
import json
import os
import sys
import tomllib
import urllib.error
import urllib.request
import uuid
from pathlib import Path

DOM_VERSIONS = (20240114, 20260812)
DEFAULT_ENDPOINT = "https://sync.koreader.rocks"


def _plain(value, name, limit):
    if not value or len(value.encode()) > limit or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError(f"{name}不能为空，不能超过 {limit} 字节，也不能含控制字符。")
    return value


def _quote(value):
    # After _plain, JSON escapes only '"' and '\\', which TOML basic strings share.
    return json.dumps(value, ensure_ascii=False)


def wifi_toml(ssid, password):
    """Return network.toml text; raise ValueError for values the firmware rejects."""
    _plain(ssid, "Wi-Fi 名称", 32)
    size = len(password.encode())
    if password:
        _plain(password, "Wi-Fi 密码", 64)
        is_hex = all(c in "0123456789abcdefABCDEF" for c in password)
        if size < 8 or (size == 64 and not is_hex):
            raise ValueError("Wi-Fi 密码需要 8 至 63 个字节；开放网络留空。")
    return f"schema_version = 1\n[wifi]\nssid = {_quote(ssid)}\npassword = {_quote(password)}\n"


def userkey(password):
    return hashlib.md5(password.encode()).hexdigest()


def sync_toml(endpoint, username, key, device_id, dom_version):
    """Return sync.toml text; raise ValueError for values the firmware rejects."""
    endpoint = endpoint.rstrip("/")
    rest = endpoint[8:]
    if (not endpoint.startswith("https://") or not rest or rest[0] == "/" or len(endpoint) > 1024 or
            any(not 32 < ord(c) < 127 or c in "@#?\\" for c in rest)):
        raise ValueError("服务地址必须以 https:// 开头，不能含空格、@、#、? 或反斜杠。")
    _plain(username, "账户", 256)
    _plain(key, "账户密钥", 256)
    _plain(device_id, "设备标识", 256)
    if dom_version not in DOM_VERSIONS:
        raise ValueError(f"dom_version 只支持 {DOM_VERSIONS[0]} 和 {DOM_VERSIONS[1]}。")
    return (f"schema_version = 1\nendpoint = {_quote(endpoint)}\nusername = {_quote(username)}\n"
            f"userkey = {_quote(key)}\ndevice_id = {_quote(device_id)}\ndom_version = {dom_version}\n")


def existing_identity(path):
    """Keep the device_id and dom_version of a readable sync.toml; the server keys records by device_id."""
    try:
        current = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None, None
    device_id, dom_version = current.get("device_id"), current.get("dom_version")
    return (device_id if isinstance(device_id, str) and device_id else None,
            dom_version if dom_version in DOM_VERSIONS else None)


def account_status(endpoint, username, key):
    """Return 'ok', 'denied', or a description of why the server could not be asked."""
    request = urllib.request.Request(endpoint.rstrip("/") + "/users/auth", headers={
        "accept": "application/vnd.koreader.v1+json", "x-auth-user": username, "x-auth-key": key})
    try:
        with urllib.request.urlopen(request, timeout=10):
            return "ok"
    except urllib.error.HTTPError as failure:
        return "denied" if failure.code in (401, 403) else f"HTTP {failure.code}"
    except (OSError, ValueError) as failure:
        return str(failure)


def write(path, text):
    temporary = path.with_name(path.name + ".tmp")
    with open(temporary, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, path)


def find_card(argument):
    if argument:
        return Path(argument)
    cards = glob.glob("/Volumes/*/PicoPage")
    if len(cards) != 1:
        raise ValueError("没有找到唯一的存储卡。请把存储卡的挂载路径作为参数传入。")
    return Path(cards[0]).parent


def main():
    parser = argparse.ArgumentParser(description="写入 Pico Page 的 Wi-Fi 和 KOReader 同步配置")
    parser.add_argument("card", nargs="?", help="存储卡挂载路径，例如 /Volumes/PICOPAGE")
    parser.add_argument("--offline", action="store_true", help="不向同步服务验证账户")
    arguments = parser.parse_args()
    try:
        card = find_card(arguments.card)
        directory = card / "PicoPage"
        if not directory.is_dir():
            raise ValueError(f"{card} 下没有 PicoPage 目录，不是 Pico Page 的存储卡。")
        print(f"存储卡：{card}")
        files = {}

        ssid = input("Wi-Fi 名称（仅 2.4 GHz；直接回车跳过）：")
        if ssid:
            files["network.toml"] = wifi_toml(ssid, getpass.getpass("Wi-Fi 密码（输入时不显示；开放网络直接回车）："))

        username = input("KOReader 同步账户（直接回车跳过）：")
        if username:
            endpoint = input(f"同步服务地址（直接回车使用 {DEFAULT_ENDPOINT}）：") or DEFAULT_ENDPOINT
            key = userkey(getpass.getpass("同步账户密码（输入时不显示）："))
            device_id, dom_version = existing_identity(directory / "sync.toml")
            files["sync.toml"] = sync_toml(endpoint, username, key, device_id or uuid.uuid4().hex.upper(),
                                           dom_version or DOM_VERSIONS[-1])
            if not arguments.offline:
                status = account_status(endpoint, username, key)
                if status == "denied":
                    raise ValueError("同步服务拒绝了这个账户或密码。没有写入任何文件。")
                print("同步服务已确认账户。" if status == "ok" else
                      f"无法从这台电脑验证账户（{status}）。配置仍会写入，请在设备上确认。")
    except ValueError as failure:
        sys.exit(str(failure))

    for name, text in files.items():
        write(directory / name, text)
        print(f"已写入 PicoPage/{name}")
    if files:
        print("请先在电脑上弹出存储卡，再在设备上结束 USB 传输。")
    else:
        print("没有输入内容，未写入任何文件。")


if __name__ == "__main__":
    main()
