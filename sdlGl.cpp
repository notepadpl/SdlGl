#include <SDL.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <cmath>
#include <stdio.h>

SDL_Window* window;
SDL_GLContext glContext;

struct Mesh {
    std::vector<float> vertices; // 3 floats per vertex
    std::vector<unsigned int> indices;
};

Mesh mesh;
GLuint program, vbo, ibo;
float angle = 0.0f;

const char* vs = R"(
    attribute vec3 aPos;
    uniform float uAngle;
    void main() {
        float c = cos(uAngle), s = sin(uAngle);
        mat3 rot = mat3(c, -s, 0, s, c, 0, 0, 0, 1);
gl_Position = vec4(rot * (aPos * 1.0), 1.0); // skaluj ręcznie

       // gl_Position = vec4(rot * (aPos - vec3(0.5)), 1.0);
    }
)";
const char* fs = R"(
    precision mediump float;
    void main() { gl_FragColor = vec4(1.0); }
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[512];
        glGetShaderInfoLog(shader, 512, NULL, info);
        printf("Shader error: %s\n", info);
    }
    return shader;
}

Mesh loadMeshFromAssimp(const char* path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals);

    Mesh m;

    if (!scene || !scene->HasMeshes()) {
        printf("Failed to load: %s\n", importer.GetErrorString());
        return m;
    }

    const aiMesh* meshData = scene->mMeshes[0];

    for (unsigned int i = 0; i < meshData->mNumVertices; ++i) {
        aiVector3D pos = meshData->mVertices[i];
        m.vertices.push_back(pos.x);
        m.vertices.push_back(pos.y);
        m.vertices.push_back(pos.z);
    }

    for (unsigned int i = 0; i < meshData->mNumFaces; ++i) {
        const aiFace& face = meshData->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            m.indices.push_back(face.mIndices[j]);
        }
    } 
// Oblicz bounding box
float minX = 1e10f, maxX = -1e10f;
float minY = 1e10f, maxY = -1e10f;
float minZ = 1e10f, maxZ = -1e10f;

for (size_t i = 0; i < mesh.vertices.size(); i += 3) {
    float x = mesh.vertices[i];
    float y = mesh.vertices[i + 1];
    float z = mesh.vertices[i + 2];

    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
}

// Oblicz środek i zakres
float centerX = (minX + maxX) / 2.0f;
float centerY = (minY + maxY) / 2.0f;
float centerZ = (minZ + maxZ) / 2.0f;
float maxExtent = std::max({ maxX - minX, maxY - minY, maxZ - minZ });

// Normalizuj
for (size_t i = 0; i < mesh.vertices.size(); i += 3) {
    mesh.vertices[i + 0] = (mesh.vertices[i + 0] - centerX) / maxExtent;
    mesh.vertices[i + 1] = (mesh.vertices[i + 1] - centerY) / maxExtent;
    mesh.vertices[i + 2] = (mesh.vertices[i + 2] - centerZ) / maxExtent;
}

    return m;
}

bool init() {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("OBJ Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              640, 480, SDL_WINDOW_OPENGL);
    glContext = SDL_GL_CreateContext(window);
    glViewport(0, 0, 640, 480);
    glClearColor(0.1f, 0.9f, 0.1f, 1.0f);

    mesh = loadMeshFromAssimp("asserts/Harpy.fbx");
    printf("Vertices: %zu, Indices: %zu\n", mesh.vertices.size()/3, mesh.indices.size()/3);

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size()*sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size()*sizeof(unsigned int), mesh.indices.data(), GL_STATIC_DRAW);

    return true;
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glUniform1f(glGetUniformLocation(program, "uAngle"), angle);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLint pos = glGetAttribLocation(program, "aPos");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glDrawElements(GL_TRIANGLES, mesh.indices.size(), GL_UNSIGNED_INT, 0);

    SDL_GL_SwapWindow(window);
}

void loop() {
    angle += 0.01f;
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) emscripten_cancel_main_loop();
    render();
}

int main() {
    if (!init()) {
        printf("Initialization failed.\n");
        return 1;
    }
    emscripten_set_main_loop(loop, 0, 1);
    return 0;
}
