import copy
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import Mock

spec = importlib.util.spec_from_file_location('cos_guard', Path(__file__).parents[1] / 'tools/cos_guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class CosGuardTests(unittest.TestCase):
    def test_role_credentials_are_read_for_each_invocation(self):
        keys = ('TENCENTCLOUD_SECRETID', 'TENCENTCLOUD_SECRETKEY', 'TENCENTCLOUD_SESSIONTOKEN')
        for secret_id in ('first', 'second'):
            values = (secret_id, 'key', 'token')
            self.assertEqual(guard.role_credentials({'environment': json.dumps(dict(zip(keys, values)))}),
                             values)
        self.assertEqual(guard.role_credentials(dict(zip(keys, ('request', 'key2', 'token2')))),
                         ('request', 'key2', 'token2'))
        with self.assertRaises(KeyError):
            guard.role_credentials({'environment': '{}'})

    def sample(self, value=0):
        return {'Period': 300, 'DataPoints': [{
            'Dimensions': [{'Name': 'bucket', 'Value': guard.BUCKET}, {'Name': 'appid', 'Value': guard.APPID}],
            'Timestamps': [1700000000], 'Values': [value],
        }]}

    def test_threshold_and_no_automatic_reopening(self):
        cos = Mock()
        result = guard.enforce(cos, lambda: self.sample(guard.LIMIT_BYTES), 1700000300)
        self.assertEqual(result['state'], 'unchanged')
        cos.put_bucket_acl.assert_not_called()
        result = guard.enforce(cos, lambda: self.sample(guard.LIMIT_BYTES + 1), 1700000300)
        self.assertEqual(result['state'], 'private')
        cos.put_bucket_acl.assert_called_once_with(Bucket=guard.BUCKET, ACL='private')

    def test_monitoring_period_alignment_includes_the_oldest_boundary_sample(self):
        now = 1700000300
        response = self.sample()
        oldest = ((now - guard.QUERY_LAG_SECONDS - guard.LOOKBACK_SECONDS)
                  // guard.PERIOD_SECONDS * guard.PERIOD_SECONDS)
        point = response['DataPoints'][0]
        point['Timestamps'] = [oldest, now - 300]
        point['Values'] = [guard.LIMIT_BYTES + 1, 0]
        cos = Mock()
        self.assertEqual(guard.enforce(cos, lambda: response, now)['state'], 'private')
        cos.put_bucket_acl.assert_called_once_with(Bucket=guard.BUCKET, ACL='private')

    def test_invalid_missing_stale_and_wrong_resource_close_access(self):
        cases = [self.sample(), self.sample(), self.sample(), self.sample(), self.sample(), self.sample()]
        cases[0]['DataPoints'] = []
        cases[1]['DataPoints'][0]['Timestamps'] = [1699990000]
        cases[2]['DataPoints'][0]['Dimensions'][0]['Value'] = 'another-bucket'
        cases[3]['DataPoints'][0]['Values'] = [float('nan')]
        cases[4]['DataPoints'][0]['Values'] = [-1]
        cases[5]['DataPoints'][0]['Values'] = [1, 2]
        for response in cases:
            with self.subTest(response=copy.deepcopy(response)):
                cos = Mock()
                with self.assertRaises(ValueError):
                    guard.enforce(cos, lambda: response, 1700000300)
                cos.put_bucket_acl.assert_called_once_with(Bucket=guard.BUCKET, ACL='private')

    def test_monitoring_and_acl_failures_remain_visible(self):
        cos = Mock()
        with self.assertRaises(TimeoutError):
            guard.enforce(cos, Mock(side_effect=TimeoutError), 1700000300)
        cos.put_bucket_acl.assert_called_once_with(Bucket=guard.BUCKET, ACL='private')
        cos.put_bucket_acl.side_effect = PermissionError
        with self.assertRaises(PermissionError):
            guard.enforce(cos, lambda: self.sample(guard.LIMIT_BYTES + 1), 1700000300)


if __name__ == '__main__':
    unittest.main()
