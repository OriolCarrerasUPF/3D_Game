/*  by Javi Agenjo 2013 UPF  javi.agenjo@gmail.com

	MAIN:
	 + This file creates the window and the game instance. 
	 + It also contains the mainloop
	 + This is the lowest level, here we access the system to create the opengl Context
	 + It takes all the events from SDL and redirect them to the game
*/

#include "framework/includes.h"

#include "framework/framework.h"
#include "graphics/mesh.h"
#include "framework/camera.h"
#include "framework/utils.h"
#include "framework/input.h"
#include "game/game.h"

#include <iostream> //to output
#include <exception>
#include <cstdlib>

// *********************************
//create a window using SDL
SDL_Window* createWindow(const char* caption, int width, int height, SDL_GLContext& context, bool fullscreen = false)
{
    int multisample = 4;
    bool retina = true; //change this to use a retina display

	//set attributes
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16); //or 24
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

	//antialiasing (disable this lines if it goes too slow)
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, multisample ); //increase to have smoother polygons

	//create the window
	SDL_Window *window = SDL_CreateWindow(caption, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|
                                          (retina ? SDL_WINDOW_ALLOW_HIGHDPI:0) |
                                          (fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0) );
	if(!window)
	{
		fprintf(stderr, "Window creation error: %s\n", SDL_GetError());
		return nullptr;
	}
  
	// Create an OpenGL context associated with the window.
	context = SDL_GL_CreateContext(window);
	if (!context)
	{
		fprintf(stderr, "OpenGL context creation error: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		return nullptr;
	}

	//get events from the queue of unprocessed events
	SDL_PumpEvents(); //without this line asserts could fail on windows

	//launch glew to extract the opengl extensions functions from the DLL
	#ifdef USE_GLEW
		const GLenum glew_status = glewInit();
		if (glew_status != GLEW_OK)
		{
			fprintf(stderr, "OpenGL extension initialization error: %s\n", glewGetErrorString(glew_status));
			SDL_GL_DeleteContext(context);
			context = nullptr;
			SDL_DestroyWindow(window);
			return nullptr;
		}
		// GLEW can leave GL_INVALID_ENUM set while probing the context.
		glGetError();
	#endif

	int window_width, window_height;
	SDL_GetWindowSize(window, &window_width, &window_height);
	std::cout << " * Window size: " << window_width << " x " << window_height << std::endl;
	std::cout << std::endl;

	return window;
}

// The application main loop
void mainLoop(Game& game)
{
	SDL_Event sdlEvent;

	long start_time = SDL_GetTicks();
	long now = start_time;
	long frames_this_second = 0;

	while (!game.must_exit)
	{
		Input::update();

		//update events
		while(!game.must_exit && SDL_PollEvent(&sdlEvent))
		{
			switch (sdlEvent.type)
			{
			case SDL_QUIT: game.must_exit = true; break; //EVENT for when the user clicks the [x] in the corner
			case SDL_MOUSEBUTTONDOWN: //EXAMPLE OF sync mouse input
				Input::mouse_state |= SDL_BUTTON(sdlEvent.button.button);
				game.onMouseButtonDown(sdlEvent.button);
				break;
			case SDL_MOUSEBUTTONUP:
				Input::mouse_state &= ~SDL_BUTTON(sdlEvent.button.button);
				game.onMouseButtonUp(sdlEvent.button);
				break;
			case SDL_MOUSEWHEEL:
				Input::mouse_wheel += sdlEvent.wheel.y;
				Input::mouse_wheel_delta += static_cast<float>(sdlEvent.wheel.y);
				game.onMouseWheel(sdlEvent.wheel);
				break;
			case SDL_MOUSEMOTION:
				Input::mouse_position.set((float)sdlEvent.motion.x, (float)sdlEvent.motion.y);
				Input::mouse_delta = Input::mouse_delta - Vector2((float)sdlEvent.motion.xrel, (float)sdlEvent.motion.yrel);
				break;
			case SDL_KEYDOWN:
				game.onKeyDown(sdlEvent.key);
				break;
			case SDL_KEYUP:
				game.onKeyUp(sdlEvent.key);
				break;
			case SDL_JOYBUTTONDOWN:
				game.onGamepadButtonDown(sdlEvent.jbutton);
				break;
			case SDL_JOYBUTTONUP:
				game.onGamepadButtonUp(sdlEvent.jbutton);
				break;
			case SDL_TEXTINPUT:
				// you can read the ASCII character from sdlEvent.text.text 
				break;
			case SDL_WINDOWEVENT:
				switch (sdlEvent.window.event) {
				case SDL_WINDOWEVENT_RESIZED: //resize opengl context
					game.onResize(sdlEvent.window.data1, sdlEvent.window.data2);
					break;
				}
			}
		}

		// All exit routes share the cleanup path in main().
		if (game.must_exit)
			break;

		// Compute delta time
		long last_time = now;
		now = SDL_GetTicks();
		double elapsed_time = (now - last_time) * 0.001; //0.001 converts from milliseconds to seconds
		double last_time_seconds = game.time;
		game.time = float(now * 0.001);
		game.elapsed_time = static_cast<float>(elapsed_time);
		game.frame++;
		frames_this_second++;
		if (int(last_time_seconds *2) != int(game.time*2)) //next half second
		{
			game.fps = (int)frames_this_second*2;
			frames_this_second = 0;
		}

		// Update game logic
		game.update(elapsed_time);

		// Render frame
		game.render();

		// Check errors in opengl only when working in debug
		#ifdef _DEBUG
			checkGLErrors();
		#endif
	}

}

int main(int argc, char **argv)
{
	std::cout << "Initiating game..." << std::endl;

	// Prepare SDL and keep the window/context under one owner: main().
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0)
	{
		std::cerr << "SDL initialization error: " << SDL_GetError() << std::endl;
		SDL_Quit();
		return EXIT_FAILURE;
	}

	bool fullscreen = false; //change this to go fullscreen
	Vector2 size(800,600);

	if(fullscreen)
		size = getDesktopSize(0);

	//create the game window (WINDOW_WIDTH and WINDOW_HEIGHT are two macros defined in includes.h)
	SDL_GLContext context = nullptr;
	SDL_Window* window = createWindow("TJE", (int)size.x, (int)size.y, context, fullscreen );
	if (!window)
	{
		SDL_Quit();
		return EXIT_FAILURE;
	}
	int window_width, window_height;
	SDL_GetWindowSize(window, &window_width, &window_height);

	Input::init(window);

	int exit_code = EXIT_SUCCESS;
	try
	{
		// The Game (and its GL-dependent resources) leaves scope before context teardown.
		Game game(window_width, window_height, window);
		mainLoop(game);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Game startup/runtime error: " << error.what() << std::endl;
		exit_code = EXIT_FAILURE;
	}

	SDL_GL_DeleteContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return exit_code;
}
