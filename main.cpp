// terrain_strip_mesh.cpp
// Compile with: g++ terrain_strip_mesh.cpp -lglfw -ldl -lGL -lX11 -lpthread -lXrandr -lXi -lm

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <vector>
#include <cmath>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <functional>

glm::mat4 model;

const int WIDTH = 1600, HEIGHT = 900;
const int GRID_W = 256, GRID_H = 256;

float yaw = -90.0f, pitch = 0.0f;
float lastX = WIDTH / 2.0f, lastY = HEIGHT / 2.0f;
bool firstMouse = true;
glm::vec3 cameraFront = glm::vec3(0, 0, -1);
glm::vec3 cameraUp = glm::vec3(0, 1, 0);
float moveSpeed = 5.0f;

// Lower amplitude for less vertical variation (flatter terrain)
float fractalNoise(float x, float y, int octaves = 6, float persistence = 0.7f) {
    float total = 0.0f;
    float frequency = 0.5f;   // Start at lower freq for big smooth hills
    float amplitude = 64.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += amplitude * (0.5f * sin(x * frequency) * cos(y * frequency) + 0.5f);
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }
    return total / maxValue; // normalized [0..1]
}

// Precomputed heightmap (global for simplicity)
std::vector<std::vector<float>> heightMap;

void generateHeightMap(int w, int h, float scale) {
    heightMap.resize(h);
    for (int y = 0; y < h; ++y) {
        heightMap[y].resize(w);
        for (int x = 0; x < w; ++x) {
            float hNoise = fractalNoise(x * scale, y * scale);
            float height = (hNoise - 0.5f) * 50.0f;  // height range [-2.5, +2.5]
            heightMap[y][x] = height;
        }
    }
}

void generateVertices(std::vector<float>& verts, int w, int h, float scale) {
    const float spacing = 10.0f; // Increase grid spacing by 10x
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float hNoise = fractalNoise(x * scale, y * scale);
            float height = (hNoise - 0.5f) * 50.0f;  // Keep your taller hills
            verts.push_back(x * spacing);
            verts.push_back(height);
            verts.push_back(y * spacing);
        }
    }
}

void generateIndices(std::vector<std::vector<unsigned int>>& strips, int w, int h) {
    for (int y = 0; y < h - 1; y++) {
        std::vector<unsigned int> strip;
        for (int x = 0; x < w; ++x) {
            int v0 = y * w + x;
            int v1 = (y + 1) * w + x;
            strip.push_back(v0);
            strip.push_back(v1);
        }
        strips.push_back(strip);
    }
}
float getInterpolatedHeight(float x, float z) {
    int x0 = static_cast<int>(x / 10.f);
    int z0 = static_cast<int>(z / 10.f);

    int x1 = x0 + 1;
    int z1 = z0 + 1;

    float tx = (x / 10.f) - x0;
    float tz = (z / 10.f) - z0;

    // Clamp
    x0 = std::clamp(x0, 0, GRID_W - 1);
    x1 = std::clamp(x1, 0, GRID_W - 1);
    z0 = std::clamp(z0, 0, GRID_H - 1);
    z1 = std::clamp(z1, 0, GRID_H - 1);

    float h00 = heightMap[z0][x0];
    float h10 = heightMap[z0][x1];
    float h01 = heightMap[z1][x0];
    float h11 = heightMap[z1][x1];

    float hx0 = glm::mix(h00, h10, tx);
    float hx1 = glm::mix(h01, h11, tx);
    return glm::mix(hx0, hx1, tz);
}

const char* vertSrc = R"(
#version 330 core
layout(location = 0) in vec3 position;
out float vHeight;
uniform mat4 mvp;
void main() {
    gl_Position = mvp * vec4(position, 1.0);
    vHeight = position.y;
})";

const char* fragSrc = R"(
#version 330 core
in float vHeight;
out vec4 fragColor;
void main() {
     float h = clamp((vHeight + 10.0) / 20.0, 0.0, 1.0); // normalize height from [-10..10] to [0..1]

    vec3 color;

    if (h < 0.3)       color = vec3(0.0, 0.0, 0.8);       // deep water - blue
    else if (h < 0.4)  color = vec3(0.0, 0.5, 1.0);       // shallow water - lighter blue
    else if (h < 0.5)  color = vec3(0.9, 0.85, 0.6);      // beach/sand - sandy color
    else if (h < 0.7)  color = vec3(0.1, 0.6, 0.1);       // grass - green
    else if (h < 0.9)  color = vec3(0.5, 0.4, 0.3);       // rock - brownish
    else               color = vec3(1.0, 1.0, 1.0);       // snow - white

    fragColor = vec4(color, 1.0);
})";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    int success;
    glGetShaderiv(s, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        std::cerr << "Shader compile error:\n" << log << "\n";
    }
    return s;
}


float getHeight(float x, float z) {
    const float spacing = 10.0f; // Must match vertex spacing

    int gridX = static_cast<int>(x / spacing);
    int gridZ = static_cast<int>(z / spacing);

    // Clamp to bounds
    gridX = std::clamp(gridX, 0, GRID_W - 1);
    gridZ = std::clamp(gridZ, 0, GRID_H - 1);

    return heightMap[gridZ][gridX];
}


class CapsuleCollider {
public:
    float posX, posY, posZ;
    float velocityY;
    float capsuleRadius;
    bool onGround;
    bool clamped;
    // Add these:
    float height;
    const float gravity = -9.8f;
    const float groundEpsilon = 0.001f;

    CapsuleCollider(float x, float y, float z, float height, float radius)
        : posX(x), posY(y), posZ(z), height(height), capsuleRadius(radius),
        velocityY(0), onGround(false), clamped(false) {
    }

    void update(float dt, std::function<float(float, float)> getTerrainHeight) {
        // Gravity
        velocityY += gravity * dt;

        // Predict vertical position
        float newY = posY + velocityY * dt;

        // Terrain height at (x, z)
        float terrainY = getTerrainHeight(posX, posZ);
        float capsuleBottom = newY - height / 2.0f;

        if (capsuleBottom <= terrainY) {
            // Landed on terrain
            newY = terrainY + height / 2.f;
            velocityY = 0.0f;
        }

        posY = newY;
    }

    //void update(float deltaTime, std::function<float(float, float)> getTerrainHeightAt) {
    //    if (!onGround) {
    //        velocityY += gravity * deltaTime;
    //        posY += velocityY * deltaTime;

    //        float terrainY = getHeight(posX, posZ);
    //        float capsuleBottom = posY - height / 2.0f;

    //        if (capsuleBottom < terrainY) {
    //            posY = terrainY + height / 2.0f;
    //            velocityY = 0.0f; // stop falling
    //        }
    //        float distToGround = capsuleBottom - terrainY;

    //        if (distToGround < groundEpsilon) {
    //            posY = terrainY + capsuleRadius + groundEpsilon;
    //            velocityY = 0;
    //            onGround = true;
    //            clamped = true;
    //        }
    //        else {
    //            onGround = false;
    //            clamped = false;
    //        }
    //    }
    //    else {
    //        velocityY = 0;
    //    }
    //}

    void moveHorizontal(float dx, float dz) {
        posX += dx;
        posZ += dz;
    }
};

class Camera {
public:
    glm::vec3 position;
    glm::vec3 forward;
    glm::vec3 up;
    glm::vec3 viewDir = glm::vec3(0, 0, -1);

    glm::mat4 getViewMatrix() const {
        return glm::lookAt(position, position + viewDir, glm::vec3(0, 1, 0));
    }
    Camera(const glm::vec3& pos)
        : position(pos), forward(0, 0, -1), up(0, 1, 0) {
    }

    void followCapsule(const CapsuleCollider& capsule, float eyeOffset) {
        position = glm::vec3(
            capsule.posX,
            capsule.posY + capsule.capsuleRadius + eyeOffset,
            capsule.posZ
        );
    }
};

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (firstMouse) {
        lastX = xpos; lastY = ypos;
        firstMouse = false;
    }

    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos; // Reversed: y-coord range bottom to top
    lastX = xpos; lastY = ypos;

    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    yaw += xoffset;
    pitch += yoffset;
    pitch = std::clamp(pitch, -89.0f, 89.0f);

    glm::vec3 dir;
    dir.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    dir.y = sin(glm::radians(pitch));
    dir.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    cameraFront = glm::normalize(dir);
}

glm::vec3 findSpawnPoint(const std::vector<std::vector<float>>& heightMap, float spacing, float capsuleHeight, float capsuleRadius);

int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }
    GLFWwindow* win = glfwCreateWindow(WIDTH, HEIGHT, "Terrain Strip Mesh", nullptr, nullptr);
    if (!win) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD\n";
        glfwDestroyWindow(win);
        glfwTerminate();
        return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glfwSwapInterval(1);
    glfwSetCursorPosCallback(win, mouse_callback);
    glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Generate heightmap ONCE at startup
    generateHeightMap(GRID_W, GRID_H, 0.15f);

    // Now generate vertices from heightmap
    std::vector<float> verts;
    generateVertices(verts, GRID_W, GRID_H, 0.15f);


    
    std::vector<std::vector<unsigned int>> strips;
    generateIndices(strips, GRID_W, GRID_H);

    // Flatten all strips into one big index vector and store offsets
    std::vector<unsigned int> allIndices;
    std::vector<GLuint> stripOffsets;
    unsigned int offset = 0;
    for (const auto& strip : strips) {
        allIndices.insert(allIndices.end(), strip.begin(), strip.end());
        stripOffsets.push_back(offset);
        offset += (GLuint)strip.size();
    }

    GLuint vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, allIndices.size() * sizeof(unsigned int), allIndices.data(), GL_STATIC_DRAW);

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glm::mat4 proj = glm::perspective(glm::radians(45.0f), WIDTH / (float)HEIGHT, 0.1f, 1000.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(32, 60, 80), glm::vec3(32, 0, 32), glm::vec3(0, 1, 0));
    model = glm::mat4(1.0f);
    glm::mat4 mvp = proj * view * model;

    GLint mvpLoc = glGetUniformLocation(prog, "mvp");

   

   
    // 3. Find a good spawn point from the heightmap
    glm::vec3 spawn = findSpawnPoint(heightMap, 10.0f, 4.0f, 1.0f);
    CapsuleCollider playerCapsule(spawn.x, spawn.y, spawn.z, 4.0f, 1.0f);
    // 4. Initialize player/capsule at that spawn
    playerCapsule.posX = spawn.x;
    playerCapsule.posY = spawn.y;  // <-- ADD THIS
    playerCapsule.posZ = spawn.z;



    glm::vec3 cameraPos(
        playerCapsule.posX,
        playerCapsule.posY + playerCapsule.capsuleRadius + 0.5f,
        playerCapsule.posZ
    );

    Camera playerCamera{ cameraPos };

    using Clock = std::chrono::high_resolution_clock;
    auto lastTime = Clock::now();

    // Look toward the center of the terrain initially
    glm::vec3 lookAt = glm::vec3(
        playerCapsule.posX + 10.0f,
        getInterpolatedHeight(playerCapsule.posX + 10.0f, playerCapsule.posZ),
        playerCapsule.posZ
    );
    cameraFront = glm::normalize(lookAt - playerCamera.position);
    while (!glfwWindowShouldClose(win)) {
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(prog);

        auto currentTime = Clock::now();
        std::chrono::duration<float> elapsed = currentTime - lastTime;
        float dt = elapsed.count(); // dt in seconds as float
        dt = std::min(dt, 0.05f); // Cap at ~20 FPS time step
        lastTime = currentTime;

        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        }

        glm::vec3 moveDir(0.0f);

        if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS)
            moveDir += glm::vec3(cameraFront.x, 0, cameraFront.z);
        if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
            moveDir -= glm::vec3(cameraFront.x, 0, cameraFront.z);
        if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
            moveDir -= glm::normalize(glm::cross(cameraFront, cameraUp));
        if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
            moveDir += glm::normalize(glm::cross(cameraFront, cameraUp));

        if (glm::length(moveDir) > 0.0f)
            moveDir = glm::normalize(moveDir);

        float speed = 10.0f;
        playerCapsule.moveHorizontal(moveDir.x * speed * dt, moveDir.z * speed * dt);

        // Use bilinear interpolation heightmap query instead of fractalNoise!
        playerCapsule.update(dt, getHeight);

        playerCamera.viewDir = cameraFront;
        playerCamera.followCapsule(playerCapsule, 0.5f);

        mvp = proj * playerCamera.getViewMatrix() * model;
        glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glBindVertexArray(vao);

        for (size_t i = 0; i < strips.size(); ++i) {
            glDrawElements(GL_TRIANGLE_STRIP, (GLsizei)strips[i].size(), GL_UNSIGNED_INT, (void*)(stripOffsets[i] * sizeof(unsigned int)));
        }

        glfwSwapBuffers(win);
        glfwPollEvents();
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}


glm::vec3 findSpawnPoint(const std::vector<std::vector<float>>& heightMap, float spacing, float capsuleHeight, float capsuleRadius) {
    int w = heightMap[0].size();
    int h = heightMap.size();

    for (int y = 5; y < h - 5; ++y) {
        for (int x = 5; x < w - 5; ++x) {
            float center = heightMap[y][x];
            float dx = std::abs(center - heightMap[y][x + 1]);
            float dz = std::abs(center - heightMap[y + 1][x]);

            // Threshold: pick spot where height doesn’t vary much
            if (dx < 1.0f && dz < 1.0f) {
                float worldX = x * spacing;
                float worldZ = y * spacing;
                float worldY = center + capsuleHeight * 0.5f + capsuleRadius + 0.1f; // start just above terrain

                return glm::vec3(worldX, worldY, worldZ);
            }
        }
    }

    // Fallback spawn if no flat spot found
    return glm::vec3(0.0f, 50.0f, 0.0f);
}