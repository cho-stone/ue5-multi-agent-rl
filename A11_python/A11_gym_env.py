#CHO_SEOKJIN

import numpy as np
from pettingzoo import ParallelEnv
from gymnasium import spaces
from A11_socket import UESocketServer
import time
import threading



class UEParallelEnv(ParallelEnv):
    #Metadata for environment identification
    metadata = {"render_modes": ["human"], "name": "A11UEEnvParallel_v0"}

    def __init__(self, env_config=None):
        #List of agents (multi-agent)
        self.agents = ["agent_0", "agent_1"]
        self.possible_agents = self.agents[:]
        self.agent_name_mapping = {name: i for i, name in enumerate(self.agents)}

        #Observation and action space setup (same for all agents)
        obs_space = spaces.Box(low=-np.inf, high=np.inf, shape=(16,), dtype=np.float32)
        act_space = spaces.Discrete(6)
        self.observation_spaces = {a: obs_space for a in self.agents}
        self.action_spaces = {a: act_space for a in self.agents}

        #Maximum possible NPC distance(for reward)
        self.max_dist = 2828.43 #Update if the map changes

        #Socket communication to UE server
        self.socket = None
        self.state = None

        #State trackers for previous step (positions, hp, alive)
        self._prev_positions = [np.zeros(3), np.zeros(3)]
        self._prev_hp = [10, 10]
        self._prev_alive = [True, True]
        self._step_count = 0
        self._max_steps = 7000

        #Socket connection attempt management
        self._connection_attempts = 0
        self._max_connection_attempts = 3
        self._socket_timeout = 5.0
        self._socket_lock = threading.Lock()

    def _connect_socket(self):
        #Attempt to connect to the UE socket server
        with self._socket_lock:
            if self._connection_attempts >= self._max_connection_attempts:
                print(f"Exceeded the maximum number of connection attempts ({self._max_connection_attempts}) - Environment initialization failed")
                raise RuntimeError("Too many socket connection attempts")
            try:
                if self.socket is None:
                    print(f"Attempting to connect to UE socket... ({self._connection_attempts + 1}/{self._max_connection_attempts})")
                    self.socket = UESocketServer()
                    print("Successful UE socket connection")
                    self._connection_attempts = 0
                    return True
            except Exception as e:
                self._connection_attempts += 1
                print(f"Socket connection failed (try {self._connection_attempts}/{self._max_connection_attempts}): {e}")
                if self._connection_attempts < self._max_connection_attempts:
                    time.sleep(1)
                    return self._connect_socket()
                else:
                    print("Socket Connection Abandoned - Environment Initialization Failed")
                    raise RuntimeError("Abandon socket connection")
            return True

    """
    Reset the environment and agents for a new episode.
    Sends a RESET command to UE and receives the initial state.
    """
    def reset(self, seed=None, options=None):
        self._step_count = 0
        if self.socket is None:
            self._connect_socket()
        try:
            self.socket.send_action("RESET\n")
            self.state = self.socket.receive_state()
            if self.state is None:
                raise RuntimeError("UE did not receive status.")           
            #Initialize previous state trackers from new state
            if "npcs" in self.state and len(self.state["npcs"]) >= 2:
                self._prev_positions = [np.array(self.state["npcs"][0]["pos"]), np.array(self.state["npcs"][1]["pos"])]
                self._prev_hp = [self.state["npcs"][0]["hp"], self.state["npcs"][1]["hp"]]
                self._prev_alive = [self.state["npcs"][0]["alive"], self.state["npcs"][1]["alive"]]
            else:
                self._prev_positions = [np.zeros(3), np.zeros(3)]
                self._prev_hp = [10, 10]
                self._prev_alive = [True, True]
            obs = self._make_obs(self.state)
            info = {agent: {} for agent in self.agents}
            return obs
        except Exception as e:
            print(f"Errors during reset: {e}")
            with self._socket_lock:
                if self.socket:
                    try:
                        self.socket.close()
                    except:
                        pass
                    self.socket = None
            raise

    """
    Step the environment forward by one tick using provided agent actions.
    Sends commands to UE, receives state and computes rewards.
    """
    def step(self, actions):
        self._step_count += 1
        if self.socket is None:
            print("Environment unavailable due to missing sockets")
            raise RuntimeError("Socket not connected")
        
        #Validate and sanitize actions for both agents
        #Default to STOP if not specified
        validated_actions = {}
        for agent in self.agents:
            action = actions.get(agent, 4)
            if not isinstance(action, (int, np.integer)) or not (0 <= action <= 5):
                action = 4
            validated_actions[agent] = int(action)

        #Map action indices to UE string commands
        action_map = ["FORWARD", "RIGHT", "BACKWARD", "LEFT", "STOP", "FIRE"]
        cmd0 = action_map[validated_actions["agent_0"]]
        cmd1 = action_map[validated_actions["agent_1"]]
        send_cmd = f"BP_AI_NPC0:{cmd0},BP_AI_NPC1:{cmd1}\n"
        try:
            self.socket.send_action(send_cmd)
            self.state = self.socket.receive_state()
            if self.state is None:
                raise RuntimeError("UE did not receive status.")
            obs = self._make_obs(self.state)
            rewards = self._compute_rewards(self.state, validated_actions)
            game_done = self.state.get("done", False)
            max_steps_reached = self._step_count >= self._max_steps
            terminated = {
                agent: game_done
                for agent in self.agents
            }
            truncated = {
                agent: max_steps_reached
                for agent in self.agents
            }
            infos = {agent: {} for agent in self.agents}

            obs = {k: np.asarray(v, dtype=np.float32) for k, v in obs.items()}
            rewards = {k: float(v) for k, v in rewards.items()}
            terminated = {k: bool(v) for k, v in terminated.items()}
            truncated = {k: bool(v) for k, v in truncated.items()}
            infos = {k: dict(v) for k, v in infos.items()}
            
            return obs, rewards, terminated, truncated, infos
        except Exception as e:
            print(f"Errors during step execution: {e}")
            with self._socket_lock:
                if self.socket:
                    try:
                        self.socket.close()
                    except:
                        pass
                    self.socket = None
            raise

    """
    Generate each agent's observation vector.
    The observation for each agent contains:
    [self position(3), self rotation(3), self hp, self alive,
        opponent position(3), opponent rotation(3), opponent hp, opponent alive]
    Total: 16 floats.
    """
    def _make_obs(self, state):
        obs = {}
        try:
            for i, agent in enumerate(self.agents):
                npc_obs = []
                if "npcs" in state and len(state["npcs"]) >= 2:
                    #my informations
                    me = state["npcs"][i]
                    #opp informations
                    other = state["npcs"][1 - i]
                    #first - my own info
                    npc_obs.extend([float(x) for x in me.get("pos", [0.0, 0.0, 0.0])])
                    npc_obs.extend([float(x) for x in me.get("rot", [0.0, 0.0, 0.0])])
                    npc_obs.append(float(me.get("hp", 10)))
                    npc_obs.append(float(me.get("alive", True)))
                    #second - opp info
                    npc_obs.extend([float(x) for x in other.get("pos", [0.0, 0.0, 0.0])])
                    npc_obs.extend([float(x) for x in other.get("rot", [0.0, 0.0, 0.0])])
                    npc_obs.append(float(other.get("hp", 10)))
                    npc_obs.append(float(other.get("alive", True)))
                else:
                    npc_obs = [0.0] * 16
                if len(npc_obs) < 16:
                    npc_obs.extend([0.0] * (16 - len(npc_obs)))
                elif len(npc_obs) > 16:
                    npc_obs = npc_obs[:16]
                obs[agent] = np.array(npc_obs, dtype=np.float32)
        except Exception as e:
            print(f"Errors during observation generation: {e}")
            obs = {agent: np.zeros(16, dtype=np.float32) for agent in self.agents}
        return obs

    """
    Compute rewards for both agents based on changes in state:
    - Bonus for damaging opponent
    - Big bonus for killing opponent and penalty for dying
    - Small movement reward
    - Bonus for getting closer, penalty for getting farther
    - Penalty for STOP action
    """
    def _compute_rewards(self, state, actions):
        rewards = {agent: 0.0 for agent in self.agents}
        try:
            #Small time reward for surviving
            for agent in self.agents:
                rewards[agent] += 0.001
            if "npcs" not in state or len(state["npcs"]) < 2:
                return rewards
            
            #Reward for damaging the opponent and track own hp
            for i, agent in enumerate(self.agents):
                if i < len(self._prev_hp) and i < len(state["npcs"]):
                    prev_hp = self._prev_hp[i]
                    cur_hp = state["npcs"][i].get("hp", 10)
                    if cur_hp < prev_hp:
                        opp_idx = 1 - i
                        if opp_idx < len(self.agents):
                            rewards[self.agents[opp_idx]] += 11 * (prev_hp - cur_hp)
                    self._prev_hp[i] = cur_hp
            
            #Bonus/penalty for killing/dying based on alive flag
            for i, agent in enumerate(self.agents):
                if i < len(self._prev_alive) and i < len(state["npcs"]):
                    alive = state["npcs"][i].get("alive", True)
                    if not alive and self._prev_alive[i]:
                        opp_idx = 1 - i
                        if opp_idx < len(self.agents):
                            rewards[self.agents[opp_idx]] += 150
                        rewards[agent] -= 20
                    self._prev_alive[i] = alive

            #Movement reward and distance-based reward
            for i, agent in enumerate(self.agents):
                #Movement rewards (rewards only your own movement)
                prev_pos = self._prev_positions[i]
                cur_pos = np.array(state["npcs"][i].get("pos", [0.0, 0.0, 0.0]))
                moved = np.linalg.norm(cur_pos - prev_pos)
                if moved > 5.0:
                    rewards[agent] += moved * 0.005
                
                #Distance change compensation (before-current relative distance change)
                other_pos = self._prev_positions[1 - i]
                my_pos = self._prev_positions[i]
                prev_dist = getattr(self, f'_prev_dist_{i}', self.max_dist)
                dist = np.linalg.norm(my_pos - other_pos)

                #Rewards for getting closer, penalties for getting further away
                rewards[agent] += (prev_dist - dist) * 0.05
                setattr(self, f'_prev_dist_{i}', dist)
                
                self._prev_positions[i] = cur_pos

            #Penalty for STOP action
            for agent, action in actions.items():
                if action == 4:
                    rewards[agent] -= 0.05

        except Exception as e:
            print(f"Errors during reward calculation: {e}")
            rewards = {agent: 0.0 for agent in self.agents}
        return {agent: float(r) for agent, r in rewards.items()}
    
    #Close the socket and cleanup environment resources
    def close(self):
        with self._socket_lock:
            if self.socket is not None:
                try:
                    self.socket.close()
                except:
                    pass
                self.socket = None
        print("Finish shutting down the environment")
