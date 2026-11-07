"""Check persisted bridge settings without importing NVDA or touching real user settings."""
import configparser
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


class ChatListSettingsTest(unittest.TestCase):
    def test_default_and_toggling_preserve_sender_setting(self):
        with tempfile.TemporaryDirectory() as folder:
            package = types.ModuleType('_settings_test')
            package.__path__ = []
            bridge = types.ModuleType('_settings_test.bridge')
            bridge.bridgeDataDir = lambda: folder
            log_handler = types.ModuleType('logHandler')
            log_handler.log = types.SimpleNamespace(error=lambda *a, **k: self.fail('settings write failed'))
            path = Path(__file__).resolve().parents[1] / 'addon/appModules/line/messageSettings.py'
            spec = importlib.util.spec_from_file_location('_settings_test.messageSettings', path)
            module = importlib.util.module_from_spec(spec)
            with patch.dict(sys.modules, {'_settings_test': package, '_settings_test.bridge': bridge,
                                          'logHandler': log_handler}):
                spec.loader.exec_module(module)
                for sender, option in [(True, None), (True, True), (False, False)]:
                    if option is None:
                        module.writeSettings(sender)
                    else:
                        module.writeSettings(sender, True, option)
                    ini = configparser.ConfigParser()
                    ini.read(Path(folder) / 'message-settings.ini', encoding='ascii')
                    self.assertEqual(ini.getboolean('messages', 'directChatSenderNames'), sender)
                    self.assertEqual(ini.getboolean('messages', 'senderOnlyWhenChanged'), option is not None)
                    self.assertEqual(ini.getboolean('chatList', 'bridgeText'), bool(option))
                    self.assertFalse((Path(folder) / 'message-settings.ini.tmp').exists())


if __name__ == '__main__':
    unittest.main()
