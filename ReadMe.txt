#A11 - Unreal Engine + Reinforcement Learning Project
[A11] Create a game in Unreal Engine and use reinforcement learning to allow NPCs to learn while playing.

===========================================================================

##Folder Descriptions

###A11_PPO_MyEnv_checkpoint:
This folder contains the PPO checkpoints saved up to episode 200.
To resume training from this point, uncomment the checkpoint loading section in A11_main_train.pyand specify the path up to checkpoint_000200.

###A11_python:
This folder contains the three Python source files used in the project
- A11_main_train.py: Launches the reinforcement learning training process using PPO.
- A11_gym_env.py: Defines the custom training environment using PettingZoo and Gymnasium.
- A11_socket.py: Handles TCP socket communication with the Unreal Engine game.

###A11_RL_Game
This is the Unreal Engine project folder that contains the game used for the RL task.

###A11_training_log.csv
This file contains training logs recorded up to episode 200, including metrics such as reward and episode length.

###A11_video.mp4
This video shows the behavior of agents during training, recorded up to episode 92.
To reduce the file size and highlight key moments, the middle portion of the video is accelerated at 16× speed.

###requirements.txt
This file lists all Python packages (with versions) used during training for reproducibility.

=====================How It Works?=========================================
1. Import the Unreal Project
Open the A11_RL_Game folder in Unreal Engine (UE5.6 or compatible version).

2. Start the Python Training Server
Open a terminal in the A11_python folder and run A11_main_train.py.
This script will launch the PPO training loop and start a socket server for communication.
(Note: I ran the Python server on WSL2 (Ubuntu), and Unreal Engine on Windows.)

3. Wait for PPO to Initialize
When you see the message "Building the PPO algorithm...", the system is ready to connect.

4. Run the Game in Unreal Engine
Start the game from the Unreal Editor. NPC agents will begin to move and interact as training progresses.