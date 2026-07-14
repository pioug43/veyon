#! /usr/bin/python3

import json
import requests
import time
import uuid

class WebAPIClient(object):

	FEATURE_SCREEN_LOCK = 'ccb535a2-1d24-4cc1-a709-8b47d2b2ac79'
	FEATURE_LOCK_INPUT_DEVICES = 'e4a77879-e544-4fec-bc18-e534f33b934c'
	FEATURE_LOGOFF_USER = '7311d43d-ab53-439e-a03a-8cb25f7ed526'
	FEATURE_POWER_DOWN = '6f5a27a0-0e2f-496e-afcc-7aae62eede10'
	FEATURE_REBOOT = '4f7d98f0-395a-4fff-b968-e49b8d0f748c'

	FEATURE_DEMO_SERVER = 'e4b6e743-1f5b-491d-9364-e091086200f4'
	FEATURE_WINDOW_DEMO_CLIENT = 'ae45c3db-dc2e-4204-ae8b-374cdab8c62c'
	FEATURE_FULLSCREEN_DEMO_CLIENT = '7b6231bd-eb89-45d3-af32-f70663b2f878'
	FEATURE_DISTRIBUTE_FILES = '4a70bd5a-fab2-4a4b-a92a-a1e81d2b75ed'
	FEATURE_COLLECT_FILES = '5a14c971-e93c-457f-97a0-0b8f1058a58e'

	ERROR_NONE = 0
	ERROR_INVALID_DATA = 1
	ERROR_INVALID_CONNECTION = 2
	ERROR_INVALID_FEATURE = 3
	ERROR_INVALID_CREDENTIALS = 4
	ERROR_AUTH_METHOD_NOT_AVAILABLE = 5
	ERROR_AUTH_FAILED = 6
	ERROR_CONN_LIMIT_REACHED = 7
	ERROR_CONN_TIMED_OUT = 8
	ERROR_UNSUPPORTED_IMAGE_FORMAT = 9
	ERROR_FRAMEBUFFER_NOT_AVAILABLE = 10
	ERROR_FRAMEBUFFER_ENCODING_ERROR = 11
	ERROR_PROTOCOL_MISMATCH = 12
	ERROR_CONNECTION_NOT_READY = 13

	class Error(RuntimeError):
		def __init__(self, error):
			error_object = json.loads(error)
			self.code = error_object['error']['code']
			self.message = error_object['error']['message']

	def __init__(self, api_url, client):
		self.api_url = api_url
		self.client = client
		self.headers = {}

	def __del__(self):
		self.close_connection()

	def is_authenticated(self):
		return self.headers and isinstance(self.headers['Connection-Uid'], str)

	@property
	def connection_uid(self):
		return self.headers.get('Connection-Uid') if self.headers else None

	def supported_authentication_methods(self):
		return self.__get('authentication/%s' % (self.client), {}).get('methods')

	def authenticate(self, method, credentials):
		auth_result = self.__post('authentication/%s' % (self.client), {}, {'method': method, 'credentials': credentials})
		if not auth_result:
			return False

		self.headers = {'Connection-Uid': auth_result.get('connection-uid')}
		return True

	def authenticate_with_key_file(self, keyname, keydata = None):
		if not keydata:
			with open('/etc/veyon/keys/private/%s/key' % (keyname), 'r') as keyfile:
				keydata = keyfile.read()

		return self.authenticate( '0c69b301-81b4-42d6-8fae-128cdd113314', {'keyname': keyname, 'keydata': keydata} )

	def authenticate_with_logon_credentials(self, username, password):
		return self.authenticate( '63611f7c-b457-42c7-832e-67d0f9281085', { "username": username, "password": password } )

	def close_connection(self):
		if self.is_authenticated():
			self.__delete('authentication/%s' % (self.client), self.headers)
			self.headers = {}

	def user_information(self):
		return self.__get('user', self.headers)

	def session_information(self):
		return self.__get('session', self.headers)

	def connection_information(self):
		return self.__get('connection', self.headers)

	def wait_for_framebuffer(self, timeout = 30):
		i = 0
		while(i < timeout):
			try:
				if self.get_image():
					return True
			except self.Error as error:
				if error.code != self.ERROR_FRAMEBUFFER_NOT_AVAILABLE:
					raise error
			time.sleep(1)
			i += 1
		return False

	def get_image(self, width = 0, height = 0, format='png', compression = 5, quality = 75):
		parameters = {'format': format, 'compression': compression, 'quality': quality}
		if width > 0:
			parameters['width'] = width
		if height > 0:
			parameters['height'] = height

		return self.__get_binary('framebuffer', self.headers, parameters)

	def send_pointer_event(self, x, y, button_mask=0):
		return self.__post('input/pointer', self.headers,
			{'x': x, 'y': y, 'buttonMask': button_mask})

	def send_key_event(self, key_code, pressed):
		return self.__post('input/key', self.headers,
			{'keyCode': key_code, 'pressed': bool(pressed)})

	def send_clipboard_text(self, text):
		return self.__post('input/clipboard', self.headers, {'text': text})

	def start_screen_broadcast(self, targets, mode='fullScreen', host=None, port=None):
		"""Broadcast this connection's screen to authenticated target clients."""
		data = {
			'targetConnectionUids': [target.connection_uid for target in targets],
			'mode': mode,
		}
		if host:
			data['host'] = host
		if port:
			data['port'] = port
		return self.__post('broadcast/start', self.headers, data)

	def stop_screen_broadcast(self, targets):
		return self.__post('broadcast/stop', self.headers, {
			'targetConnectionUids': [target.connection_uid for target in targets],
		})

	def available_features(self):
		return self.__get('feature', self.headers)

	def is_feature_active(self, feature):
		return self.__get('feature/%s' % (feature), self.headers).get('active')

	def start_feature(self, feature, arguments = None):
		arguments = arguments or {}
		return self.__put('feature/%s' % (feature), self.headers, {'active': True, 'arguments': arguments})

	def stop_feature(self, feature, arguments = None):
		arguments = arguments or {}
		return self.__put('feature/%s' % (feature), self.headers, {'active': False, 'arguments': arguments})

	def control_feature(self, feature, operation, arguments=None, targets=None):
		data = {'operation': operation, 'arguments': arguments or {}}
		if targets:
			data['targetConnectionUids'] = [target.connection_uid for target in targets]
		return self.__post('feature/%s/control' % feature, self.headers, data)

	def distribute_files(self, files, targets=None, destination_directory=None,
			overwrite=False, open_files=False, open_folder=False):
		"""Distribute files located on the WebAPI server to connected computers."""
		arguments = {
			'files': list(files),
			'overwriteExistingFile': bool(overwrite),
			'openFileInApplication': bool(open_files),
			'openTransferFolder': bool(open_folder),
		}
		if destination_directory:
			arguments['destinationDirectory'] = destination_directory
		self.control_feature(self.FEATURE_DISTRIBUTE_FILES, 'initialize', arguments, targets)
		return self.control_feature(self.FEATURE_DISTRIBUTE_FILES, 'start', {}, targets)

	def stop_file_distribution(self):
		return self.control_feature(self.FEATURE_DISTRIBUTE_FILES, 'stop')

	def collect_files(self, source_directory=None, file_pattern=None, recursive=None,
			destination_directory=None, targets=None):
		arguments = {}
		if source_directory is not None:
			arguments['sourceDirectory'] = source_directory
		if file_pattern is not None:
			arguments['filePattern'] = file_pattern
		if recursive is not None:
			arguments['collectRecursively'] = bool(recursive)
		if destination_directory is not None:
			arguments['destinationDirectory'] = destination_directory
		return self.control_feature(self.FEATURE_COLLECT_FILES, 'start', arguments, targets)

	def stop_file_collection(self):
		return self.control_feature(self.FEATURE_COLLECT_FILES, 'stop')

	def logoff_user(self):
		return self.start_feature(self.FEATURE_LOGOFF_USER)

	def powerdown_computer(self):
		return self.start_feature(self.FEATURE_POWER_DOWN)

	def reboot_computer(self):
		return self.start_feature(self.FEATURE_REBOOT)

	def lock_screen(self):
		return self.start_feature(self.FEATURE_SCREEN_LOCK)

	def unlock_screen(self):
		return self.stop_feature(self.FEATURE_SCREEN_LOCK)

	def lock_input_devices(self):
		return self.start_feature(self.FEATURE_LOCK_INPUT_DEVICES)

	def unlock_input_devices(self):
		return self.stop_feature(self.FEATURE_LOCK_INPUT_DEVICES)

	def start_demo_server(self):
		token = str(uuid.uuid4())
		self.start_feature(self.FEATURE_DEMO_SERVER, { 'demoAccessToken': token })
		return token

	def stop_demo_server(self):
		return self.stop_feature(self.FEATURE_DEMO_SERVER)

	def start_demo_client(self, demo_server, demo_token, fullscreen = True):
		if fullscreen:
			feature = self.FEATURE_FULLSCREEN_DEMO_CLIENT
		else:
			feature = self.FEATURE_WINDOW_DEMO_CLIENT

		return self.start_feature(feature, { 'demoAccessToken': demo_token, 'demoServerHost': demo_server })

	def stop_demo_client(self):
		return self.stop_feature(self.FEATURE_FULLSCREEN_DEMO_CLIENT) and self.stop_feature(self.FEATURE_WINDOW_DEMO_CLIENT)

	# private helper methods
	def __get(self, method, headers, data = None):
		data = data or {}
		response = requests.get(self.api_url + method, headers=headers, params=data)

		if response.status_code != 200:
			print("error:", response.text)
			return None

		return response.json()

	def __get_binary(self, method, headers, data = None):
		data = data or {}
		response = requests.get(self.api_url + method, headers=headers, params=data)

		if response.status_code != 200:
			raise self.Error(response.text)
			return None

		return response.content

	def __post(self, method, headers, data = None):
		data = data or {}
		response = requests.post(self.api_url + method, headers=headers, json=data)

		if response.status_code != 200:
			raise self.Error(response.text)

		return response.json()

	def __put(self, method, headers, data = None):
		data = data or {}
		response = requests.put(self.api_url + method, headers=headers, json=data)

		if response.status_code != 200:
			raise self.Error(response.text)

		return response.json()

	def __delete(self, method, headers = {}):
		response = requests.delete(self.api_url + method, headers=headers)

		if response.status_code != 200:
			raise self.Error(response.text)

		return response.json()
