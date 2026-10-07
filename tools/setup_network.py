#!/usr/bin/env python3
"""Write PicoPage/network.toml and PicoPage/sync.toml on a mounted Pico Page TF card.

A new Wi-Fi network is added to the networks already saved, up to five; a saved name gets the new
password.

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
import re
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


WIFI_LIMIT = 5


def _wifi_entry(ssid, password):
    _plain(ssid, "Wi-Fi 名称", 32)
    size = len(password.encode())
    if password:
        _plain(password, "Wi-Fi 密码", 64)
        is_hex = all(c in "0123456789abcdefABCDEF" for c in password)
        if size < 8 or (size == 64 and not is_hex):
            raise ValueError("Wi-Fi 密码需要 8 至 63 个字节；开放网络留空。")
    return {"ssid": ssid, "password": password}


def _networks_from_text(text):
    """Validate the same network shape and value limits as the firmware parser."""
    invalid = "现有 PicoPage/network.toml 格式不受支持。请先检查或删除它；没有写入任何文件。"
    if len(text.encode("utf-8")) > 4096:
        raise ValueError(invalid)
    try:
        current = tomllib.loads(text)
    except ValueError:
        raise ValueError(invalid) from None
    if set(current) - {"schema_version", "wifi"}:
        raise ValueError(invalid)
    version, wifi = current.get("schema_version"), current.get("wifi")
    if type(version) is not int:
        raise ValueError(invalid)
    if version == 1 and isinstance(wifi, dict):
        entries = [wifi]
    elif version == 2 and isinstance(wifi, list):
        entries = wifi
    elif version == 2 and wifi is None:
        entries = []
    else:
        raise ValueError(invalid)
    if len(entries) > WIFI_LIMIT:
        raise ValueError(invalid)
    networks = []
    seen = set()
    for entry in entries:
        allowed = {"ssid", "password"} | ({"last_connected"} if version == 2 else set())
        if not isinstance(entry, dict) or set(entry) - allowed or not isinstance(entry.get("ssid"), str) or not isinstance(entry.get("password"), str):
            raise ValueError(invalid)
        try:
            _wifi_entry(entry["ssid"], entry["password"])
        except ValueError:
            raise ValueError(invalid) from None
        if entry["ssid"] in seen:
            raise ValueError(invalid)
        seen.add(entry["ssid"])
        saved = {"ssid": entry["ssid"], "password": entry["password"]}
        if "last_connected" in entry:
            last = entry["last_connected"]
            if type(last) is not int or not 0 <= last < 1 << 63:
                raise ValueError(invalid)
            if last:
                saved["last_connected"] = last
        networks.append(saved)
    return networks


def existing_networks(path):
    """Return saved networks, [] if absent; reject a file the firmware cannot use."""
    try:
        return _networks_from_text(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return []
    except (OSError, UnicodeError):
        raise ValueError("现有 PicoPage/network.toml 无法读取。请先检查或删除它；没有写入任何文件。") from None


def wifi_toml(ssid, password, networks=()):
    """Return schema 2 network.toml text with ssid added to networks, or its password replaced when
    already saved; raise ValueError for values the firmware rejects or a sixth network."""
    entry = _wifi_entry(ssid, password)
    merged = [dict(saved) for saved in networks]
    for saved in merged:
        if saved["ssid"] == ssid:
            saved["password"] = password
            break
    else:
        if len(merged) >= WIFI_LIMIT:
            raise ValueError(f"已保存 {WIFI_LIMIT} 个网络。请先在设备的手机配置页删除一个；没有写入任何文件。")
        merged.append(entry)
    text = "schema_version = 2\n"
    for saved in merged:
        text += f"\n[[wifi]]\nssid = {_quote(saved['ssid'])}\npassword = {_quote(saved['password'])}\n"
        if saved.get("last_connected"):
            text += f"last_connected = {saved['last_connected']}\n"
    return text


def _replace_password(line, old_password, new_password):
    """Replace a single-line TOML string value and retain its spacing and trailing comment."""
    match = re.match(r"^(\s*password\s*=\s*)(['\"])", line)
    if not match or line[match.end() - 1:match.end() + 2] == match.group(2) * 3:
        return None
    quote = match.group(2)
    end = match.end()
    while end < len(line) and line[end] not in "\r\n":
        if quote == '"' and line[end] == "\\":
            end += 2
            continue
        if line[end] == quote:
            literal = line[match.end() - 1:end + 1]
            try:
                valid = tomllib.loads("password = " + literal)["password"] == old_password
            except ValueError:
                valid = False
            if valid and (not line[end + 1:].strip() or line[end + 1:].lstrip().startswith("#")):
                return line[:match.end() - 1] + _quote(new_password) + line[end + 1:]
            return None
        end += 1
    return None


def edited_wifi_toml(source, ssid, password):
    """Edit supported source lines without removing comments or other entries."""
    _wifi_entry(ssid, password)
    current = _networks_from_text(source)
    desired = [dict(entry) for entry in current]
    target = next((index for index, entry in enumerate(desired) if entry["ssid"] == ssid), None)
    if target is None:
        if len(desired) >= WIFI_LIMIT:
            raise ValueError(f"已保存 {WIFI_LIMIT} 个网络。请先在设备的手机配置页删除一个；没有写入任何文件。")
        desired.append({"ssid": ssid, "password": password})
    else:
        desired[target]["password"] = password
    unsupported = "现有 PicoPage/network.toml 的写法无法保留注释并修改；没有写入任何文件。"
    lines = source.splitlines(keepends=True)
    version = tomllib.loads(source)["schema_version"]
    header = r"\s*\[\[\s*wifi\s*\]\]\s*(?:#.*)?"
    headings = [index for index, line in enumerate(lines) if re.fullmatch(header, line.rstrip("\r\n"))]
    if version == 1:
        legacy = [index for index, line in enumerate(lines) if
                  re.fullmatch(r"\s*\[\s*wifi\s*\]\s*(?:#.*)?", line.rstrip("\r\n"))]
        versions = [(index, re.match(r"^(\s*schema_version\s*=\s*)1(?=\s|#|$)", line))
                    for index, line in enumerate(lines)]
        versions = [(index, match) for index, match in versions if match]
        if headings or len(legacy) != 1 or len(versions) != 1:
            raise ValueError(unsupported)
        index, match = versions[0]
        lines[index] = lines[index][:match.start(0)] + match.group(1) + "2" + lines[index][match.end():]
        index = legacy[0]
        lines[index] = lines[index].replace("[wifi]", "[[wifi]]", 1)
        headings = [index]
    if len(headings) != len(current):
        raise ValueError(unsupported)
    if target is not None:
        first = headings[target] + 1
        last = headings[target + 1] if target + 1 < len(headings) else len(lines)
        passwords = [index for index in range(first, last) if re.match(r"^\s*password\s*=", lines[index])]
        if len(passwords) != 1:
            raise ValueError(unsupported)
        replacement = _replace_password(lines[passwords[0]], current[target]["password"], password)
        if replacement is None:
            raise ValueError(unsupported)
        lines[passwords[0]] = replacement
        edited = "".join(lines)
    else:
        edited = "".join(lines)
        newline = "\r\n" if "\r\n" in edited else "\n"
        if edited and not edited.endswith(("\n", "\r")):
            edited += newline
        edited += (newline + "[[wifi]]" + newline + "ssid = " + _quote(ssid) + newline +
                   "password = " + _quote(password) + newline)
    if _networks_from_text(edited) != desired:
        raise ValueError(unsupported)
    return edited


def prepared_wifi_edit(path, ssid, password):
    """Return the original bytes and proposed source; None means the file is absent."""
    try:
        original = path.read_bytes()
    except FileNotFoundError:
        return None, wifi_toml(ssid, password)
    except OSError:
        raise ValueError("现有 PicoPage/network.toml 无法读取；没有写入任何文件。") from None
    try:
        source = original.decode("utf-8")
    except UnicodeError:
        raise ValueError("现有 PicoPage/network.toml 不是 UTF-8；没有写入任何文件。") from None
    return original, edited_wifi_toml(source, ssid, password)


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


_UNCONDITIONAL = object()


def write(path, text, expected=_UNCONDITIONAL):
    def unchanged():
        if expected is _UNCONDITIONAL:
            return
        try:
            current = path.read_bytes()
        except FileNotFoundError:
            current = None
        if current != expected:
            raise ValueError("PicoPage/network.toml 在读取后已改变；没有覆盖它。请重新运行配置工具。")

    unchanged()
    temporary = path.with_name(path.name + ".tmp")
    try:
        with open(temporary, "w", encoding="utf-8", newline="\n") as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        unchanged()
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


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
        expected = {}

        ssid = input("Wi-Fi 名称（仅 2.4 GHz；直接回车跳过）：")
        if ssid:
            original, edited = prepared_wifi_edit(directory / "network.toml", ssid,
                                                   getpass.getpass("Wi-Fi 密码（输入时不显示；开放网络直接回车）："))
            files["network.toml"] = edited
            expected["network.toml"] = original

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

    try:
        for name, text in files.items():
            write(directory / name, text, expected.get(name, _UNCONDITIONAL))
            print(f"已写入 PicoPage/{name}")
    except ValueError as failure:
        sys.exit(str(failure))
    if files:
        print("请先在电脑上弹出存储卡，再在设备上结束 USB 传输。")
    else:
        print("没有输入内容，未写入任何文件。")


if __name__ == "__main__":
    main()
