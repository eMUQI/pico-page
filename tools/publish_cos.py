#!/usr/bin/env python3
"""Publish verified firmware images to COS with short-lived GitHub OIDC credentials."""
import argparse
import json
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


def publish(cos, directory):
    """Upload immutable images with inherited ACLs and verify every byte before site deployment."""
    version = ota_manifest.check_directory(directory, ORIGIN)
    if not version:
        raise ValueError('Firmware manifest is required')
    for path in sorted(directory.glob('*.bin')):
        data = path.read_bytes()
        try:
            cos.put_object(Bucket=BUCKET, Key=path.name, Body=data, EnableMD5=True,
                           ContentType='application/octet-stream',
                           Metadata={'x-cos-forbid-overwrite': 'true'})
        except Exception as error:
            # A repeat deployment may reuse identical images but cannot replace a published version.
            if getattr(error, 'get_error_code', lambda: None)() != 'FileAlreadyExists':
                raise
        response = cos.get_object(Bucket=BUCKET, Key=path.name)
        stream = response['Body'].get_raw_stream()
        try:
            if stream.read(len(data) + 1) != data:
                raise ValueError('COS firmware differs from the release: ' + path.name)
        finally:
            stream.close()
    return version


def verify_public(directory):
    """Verify public COS bytes; permit HTTP 403 only when all deployed firmware stays identical."""
    def compare(base, path):
        request = urllib.request.Request(base + path.name, headers={
            'User-Agent': 'PicoPage deployment verification/1.0'})
        expected = path.read_bytes()
        with urllib.request.urlopen(request, timeout=30) as response:
            if response.status != 200 or response.read(len(expected) + 1) != expected:
                raise ValueError('Public firmware differs from the release: ' + path.name)

    images = sorted(directory.glob('*.bin'))
    try:
        for path in images:
            compare(ORIGIN, path)
    except urllib.error.HTTPError as error:
        if error.code != 403:
            raise
        error.close()
        # A closed download origin must not prevent page updates that preserve published firmware.
        manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
        if not (directory / ota_manifest.full_name(manifest['version'])).is_file():
            raise ValueError('Unchanged firmware verification requires the full-flash image')
        compare(SITE, directory / 'manifest.json')
        for path in images:
            compare(SITE, path)
        print('::warning::COS public downloads return HTTP 403. Only unchanged firmware was '
              'verified against production; download access remains blocked.', flush=True)
        return 'unchanged'
    return 'public'


def github_credentials():
    """Exchange a GitHub job identity for a 15-minute role session without storing cloud keys."""
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

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    # Reject invalid release inputs before requesting any cloud credentials.
    if not ota_manifest.check_directory(args.directory, ORIGIN):
        raise ValueError('Firmware manifest is required')
    credential = github_credentials()
    client = CosS3Client(CosConfig(Region=REGION, SecretId=credential['TmpSecretId'],
                                  SecretKey=credential['TmpSecretKey'], Token=credential['Token'],
                                  Scheme='https', Timeout=30, VerifySSL=True), retry=0)
    print('Published COS firmware ' + publish(client, args.directory))
    print('Firmware download verification: ' + verify_public(args.directory))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        code = getattr(error, 'get_error_code', lambda: None)() or type(error).__name__
        sys.exit('COS publication failed: ' + code)
