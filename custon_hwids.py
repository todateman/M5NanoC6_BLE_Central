Import("env")

board_config = env.BoardConfig()
board_config.update("frameworks", ["arduino", "espidf"])