#!/usr/bin/env python3
"""Publish verified firmware images to COS with short-lived GitHub OIDC credentials."""
import argparse
import json
import logging
import os
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request

import ota_manifest

BUCKET = 'pico-page-firmware-test-1258873179'
REGION = 'ap-guangzhou'
ORIGIN = f'https://{BUCKET}.cos.{REGION}.myqcloud.com/'
SITE = 'https://picopage.wulu.zone/firmware/'
PROVIDER = 'PicoPageGitHub'
AUDIENCE = 'pico-page-cos'
ROLE_ARN = 'qcs::cam::uin/100009571345:roleName/PicoPageCosPublisher'


def ota_channels(directory):
    channels = [(directory, 'legacy')]
    current = directory / 'ota-v2'
    if current.exists():
        channels.append((current, 'ota7'))
    return channels


def publish(cos, directory, *, expected_layout='legacy'):
    version = ota_manifest.check_directory(directory, ORIGIN, expected_layout=expected_layout, require_full=True)
    for path in sorted(directory.glob('*.bin')):
        data = path.read_bytes()
        print('Checking authenticated COS firmware ' + path.name, flush=True)
        try:
            response = cos.get_object(Bucket=BUCKET, Key=path.name)
        except Exception as error:
            if getattr(error, 'get_error_code', lambda: None)() != 'NoSuchKey':
                raise
            print('Uploading COS firmware ' + path.name, flush=True)
            try:
                with path.open('rb') as body:
                    cos.put_object(Bucket=BUCKET, Key=path.name, Body=body, EnableMD5=True,
                                   ContentType='application/octet-stream',
                                   Metadata={'x-cos-forbid-overwrite': 'true'})
            except Exception as error:
                # COS rejects the conditional PUT if another publisher created the object after the GET.
                if getattr(error, 'get_error_code', lambda: None)() != 'FileAlreadyExists':
                    raise
            response = cos.get_object(Bucket=BUCKET, Key=path.name)
        print('Verifying authenticated COS firmware ' + path.name, flush=True)
        stream = response['Body'].get_raw_stream()
        try:
            if stream.read(len(data) + 1) != data:
                raise ValueError('COS firmware differs from the release: ' + path.name)
        finally:
            stream.close()
    return version


def verify_public(directory):
    def compare(url, path):
        request = urllib.request.Request(url, headers={
            'User-Agent': 'PicoPage deployment verification/1.0'})
        expected = path.read_bytes()
        with urllib.request.urlopen(request, timeout=30) as response:
            if response.status != 200 or response.read(len(expected) + 1) != expected:
                raise ValueError('Public firmware differs from the release: ' + path.name)

    channels = ota_channels(directory)
    images = [image for bundle, _ in channels for image in sorted(bundle.glob('*.bin'))]
    try:
        for path in images:
            compare(ORIGIN + path.name, path)
    except urllib.error.HTTPError as error:
        if error.code != 403:
            raise
        error.close()
        manifests = []
        for bundle, _ in channels:
            manifest_path = bundle / 'manifest.json'
            manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
            if not (bundle / ota_manifest.full_name(manifest['version'])).is_file():
                raise ValueError('Unchanged firmware verification requires the full-flash image')
            manifests.append(manifest_path)
        for path in manifests + images:
            compare(SITE + path.relative_to(directory).as_posix(), path)
        print('::warning::COS public downloads return HTTP 403. Only unchanged firmware was '
              'verified against production; download access remains blocked.', flush=True)
        return 'unchanged'
    return 'public'


def github_credentials():
    from tencentcloud.common.common_client import CommonClient
    from tencentcloud.common.profile.client_profile import ClientProfile

    url = os.environ['ACTIONS_ID_TOKEN_REQUEST_URL']
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != 'https' or not (parts.hostname or '').endswith('.actions.githubusercontent.com'):
        raise ValueError('Unexpected GitHub identity endpoint')
    url += ('&' if parts.query else '?') + urllib.parse.urlencode({'audience': AUDIENCE})
    request = urllib.request.Request(url, headers={
        'Authorization': 'Bearer ' + os.environ['ACTIONS_ID_TOKEN_REQUEST_TOKEN']})
    with urllib.request.urlopen(request, timeout=15) as response:
        token = json.loads(response.read(256 * 1024))['value']
    print('::add-mask::' + token, flush=True)
    profile = ClientProfile()
    profile.httpProfile.reqTimeout = 15
    client = CommonClient(service='sts', version='2018-08-13', credential=None,
                          region=REGION, profile=profile)
    result = client.call_json('AssumeRoleWithWebIdentity', {
        'ProviderId': PROVIDER, 'WebIdentityToken': token, 'RoleArn': ROLE_ARN,
        'RoleSessionName': 'pico-page-' + os.environ['GITHUB_RUN_ID'], 'DurationSeconds': 900,
    }, options={'SkipSign': True})['Response']['Credentials']
    for value in result.values():
        print('::add-mask::' + value, flush=True)
    return result


def main():
    from qcloud_cos import CosConfig, CosS3Client

    logger = logging.getLogger('qcloud_cos.cos_client')
    logger.setLevel(logging.ERROR)
    logger.addHandler(logging.StreamHandler())

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    channels = ota_channels(args.directory)
    for bundle, layout in channels:
        ota_manifest.check_directory(bundle, ORIGIN, expected_layout=layout, require_full=True)
    print('Requesting COS publisher credentials', flush=True)
    credential = github_credentials()
    client = CosS3Client(CosConfig(Region=REGION, SecretId=credential['TmpSecretId'],
                                  SecretKey=credential['TmpSecretKey'], Token=credential['Token'],
                                  Scheme='https', Timeout=30, VerifySSL=True,
                                  EnableOldDomain=False, EnableInternalDomain=False), retry=0)
    for bundle, layout in channels:
        print('Published COS firmware ' + publish(client, bundle, expected_layout=layout))
    print('Firmware download verification: ' + verify_public(args.directory))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        code = getattr(error, 'get_error_code', lambda: None)() or type(error).__name__
        if error.__context__ is not None:
            code += ': ' + type(error.__context__).__name__
            for reason in ('CERTIFICATE_VERIFY_FAILED', 'UNEXPECTED_EOF_WHILE_READING',
                           'EOF occurred in violation of protocol', 'TLSV1_ALERT_PROTOCOL_VERSION'):
                if reason in str(error.__context__):
                    code += ': ' + reason
        sys.exit('COS publication failed: ' + code)
