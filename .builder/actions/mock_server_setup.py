"""
Setup local mock server for tests
"""

import Builder

import os
import sys
import subprocess
import atexit


class MockServerSetup(Builder.Action):
    """
    Set up this machine for running the mock server test

    This action should be run in the 'pre_build_steps' or 'build_steps' stage.
    """

    def run(self, env):
        self.env = env
        python_path = sys.executable
        # install dependency for mock server
        self.env.shell.exec(python_path,
                            '-m', 'pip', 'install', 'h11', 'trio', check=True)
        # check the deps can be import correctly
        self.env.shell.exec(python_path,
                            '-c', 'import h11, trio', check=True)

        # set cmake flag so mock server tests are enabled
        env.project.config['cmake_args'].append(
            '-DENABLE_MOCK_SERVER_TESTS=ON')

        base_dir = os.path.dirname(os.path.realpath(__file__))
        dir = os.path.join(base_dir, "..", "..", "tests", "mock_s3_server")
        os.chdir(dir)

        p = subprocess.Popen([python_path, "mock_s3_server.py"])

        @atexit.register
        def close_mock_server():
            p.terminate()
