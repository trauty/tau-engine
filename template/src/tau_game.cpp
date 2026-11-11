#include "tau/game.h"
#include "tau/log.h"

// TAU_REFLECT: components, TAU_ON_LOAD: systems and input, TAU_ON_PLAY/STOP: game state
TAU_ON_PLAY() { TAU_LOG_INFO("GAME", "Hello from ${NAME}"); }
