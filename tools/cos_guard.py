#!/usr/bin/env python3
from datetime import datetime, timedelta, timezone
import json
import math
import time

BUCKET = 'pico-page-firmware-test-1258873179'
REGION = 'ap-guangzhou'
APPID = '1258873179'
LIMIT_BYTES = 100 * 1024 * 1024
PERIOD_SECONDS = 300
LOOKBACK_SECONDS = 1800
QUERY_LAG_SECONDS = 120
MAX_AGE_SECONDS = 900


def peak_bytes(response, now):
    points = response.get('DataPoints')
    if response.get('Period') != PERIOD_SECONDS or not isinstance(points, list) or len(points) != 1:
        raise ValueError('Missing five-minute COS monitoring data')
    point = points[0]
    dimensions = {item['Name']: item['Value'] for item in point.get('Dimensions', [])}
    if dimensions.get('bucket') != BUCKET or dimensions.get('appid') != APPID:
        raise ValueError('Unexpected monitoring resource')
    timestamps, values = point.get('Timestamps', []), point.get('Values', [])
    if not values or len(values) != len(timestamps) or len(values) > 16:
        raise ValueError('Invalid monitoring sample count')
    for timestamp, value in zip(timestamps, values):
        if (not isinstance(timestamp, (int, float)) or not math.isfinite(timestamp)
                or timestamp < now - LOOKBACK_SECONDS - QUERY_LAG_SECONDS - PERIOD_SECONDS
                or timestamp > now + 60
                or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0):
            raise ValueError('Invalid monitoring sample')
    if max(timestamps) < now - MAX_AGE_SECONDS:
        raise ValueError('COS monitoring data is stale')
    return max(values)


def enforce(cos, fetch_metrics, now):
    try:
        peak = peak_bytes(fetch_metrics(), now)
    except Exception:
        cos.put_bucket_acl(Bucket=BUCKET, ACL='private')
        raise
    if peak > LIMIT_BYTES:
        cos.put_bucket_acl(Bucket=BUCKET, ACL='private')
        return {'bucket': BUCKET, 'state': 'private', 'peak_bytes': peak}
    return {'bucket': BUCKET, 'state': 'unchanged', 'peak_bytes': peak}


def role_credentials(context):
    keys = ('TENCENTCLOUD_SECRETID', 'TENCENTCLOUD_SECRETKEY', 'TENCENTCLOUD_SESSIONTOKEN')
    values = context if all(context.get(key) for key in keys) else json.loads(context['environment'])
    return tuple(values[key] for key in keys)


def main_handler(event, context):
    """Accept no resource or threshold overrides and never log role credentials."""
    from qcloud_cos import CosConfig, CosS3Client
    from tencentcloud.common.credential import Credential
    from tencentcloud.common.profile.client_profile import ClientProfile
    from tencentcloud.monitor.v20180724 import models, monitor_client

    secret_id, secret_key, token = role_credentials(context)
    cos = CosS3Client(CosConfig(Region=REGION, SecretId=secret_id, SecretKey=secret_key,
                               Token=token, Scheme='https', Timeout=4, VerifySSL=True), retry=0)
    profile = ClientProfile()
    profile.httpProfile.reqTimeout = 4
    monitor = monitor_client.MonitorClient(Credential(secret_id, secret_key, token), REGION, profile)
    now = time.time()
    end = datetime.fromtimestamp(now - QUERY_LAG_SECONDS, timezone.utc)
    request = models.GetMonitorDataRequest()
    request.from_json_string(json.dumps({
        'Namespace': 'QCE/COS', 'MetricName': 'InternetTraffic', 'Period': PERIOD_SECONDS,
        'StartTime': (end - timedelta(seconds=LOOKBACK_SECONDS)).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'EndTime': end.strftime('%Y-%m-%dT%H:%M:%SZ'),
        'Instances': [{'Dimensions': [{'Name': 'appid', 'Value': APPID},
                                      {'Name': 'bucket', 'Value': BUCKET}]}],
    }))
    result = enforce(cos, lambda: json.loads(monitor.GetMonitorData(request).to_json_string()), now)
    print(json.dumps(result))
    return result
