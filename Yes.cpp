#include <SDL.h>
#include <SDL_image.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <string>
#include <cmath>
#include <stdio.h>

// --- Globalne zmienne ---
SDL_Window* window;
SDL_GLContext glContext;
GLuint program;
float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;

// --- Struktury ---
struct Material {
    GLuint diffuse = 0;
};

struct MeshData {
    std::vector<float> vertices; // Pozycja(3), UV(2)
    std::vector<unsigned int> indices;
};

class Model {
public:
    GLuint vbo, ibo;
    size_t indexCount;
    Material material;

    void render(GLuint program, float rotX, float rotY) {
        glUseProgram(program);

        glUniform1f(glGetUniformLocation(program, "rotX"), rotX);
        glUniform1f(glGetUniformLocation(program, "rotY"), rotY);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, material.diffuse);
        glUniform1i(glGetUniformLocation(program, "tex"), 0);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

        GLint posLoc = glGetAttribLocation(program, "aPos");
        GLint uvLoc = glGetAttribLocation(program, "aUV");

        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)0);

        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)(sizeof(float) * 3));

        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, (void*)0);
    }

    void cleanup() {
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ibo);
        glDeleteTextures(1, &material.diffuse);
    }
};

Model harpyModel;

// --- Uproszczone shadery, bez oświetlenia ---
const char* vs = R"(
attribute vec3 aPos;
attribute vec2 aUV;

varying vec2 vUV;

uniform float rotX, rotY;

void main(){
    float cx = cos(rotX), sx = sin(rotX);
    float cy = cos(rotY), sy = sin(rotY);
    mat3 Rx = mat3(1, 0, 0, 0, cx, -sx, 0, sx, cx);
    mat3 Ry = mat3(cy, 0, sy, 0, 1, 0, -sy, 0, cy);
    vec3 p = Ry * Rx * aPos;
    gl_Position = vec4(p * 0.1, 1.0);
    vUV = aUV;
}
)";

const char* fs = R"(
precision mediump float;

uniform sampler2D tex;
varying vec2 vUV;

void main() {
    gl_FragColor = texture2D(tex, vUV);
}
)";

// --- Shader utils (bez zmian) ---
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

// --- Tekstury (bez zmian, ładowanie tylko dyfuzyjnej) ---
GLuint loadTextureFromMaterial(aiMaterial* mat, aiTextureType type, const std::string& directory) {
    if (mat->GetTextureCount(type) > 0) {
        aiString path;
        mat->GetTexture(type, 0, &path);
        printf("Assimp path: %s\n", path.C_Str());
        
        std::string fullPath = directory + "/" + std::string(path.C_Str());

        SDL_Surface* surface = IMG_Load(fullPath.c_str());
        if (!surface) {
            printf("Failed to load texture: %s\n", fullPath.c_str());
            return 0;
        } else {
            printf("Loaded texture: %s\n", fullPath.c_str());
        }

        GLuint texID;
        glGenTextures(1, &texID);
        glBindTexture(GL_TEXTURE_2D, texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GLenum format = (surface->format->BytesPerPixel == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, surface->w, surface->h, 0, format, GL_UNSIGNED_BYTE, surface->pixels);
        glGenerateMipmap(GL_TEXTURE_2D);

        SDL_FreeSurface(surface);
        return texID;
    }
    return 0;
}

Material loadMaterial(const aiScene* scene, const aiMesh* mesh, const std::string& directory) {
    Material mat;
    if (!scene->HasMaterials()) return mat;
    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    mat.diffuse = loadTextureFromMaterial(material, aiTextureType_DIFFUSE, directory);
    return mat;
}

// --- Wczytywanie modelu (uproszczone) ---
Model loadModel(const char* meshPath, const std::string& textureDirectory) {
    Model model;
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(meshPath, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals); // Usunięto GenNormals i CalcTangentSpace
    if (!scene || !scene->HasMeshes()) {
        printf("Model load failed: %s\n", importer.GetErrorString());
        return model;
    }

    MeshData meshData;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        unsigned int offset = meshData.vertices.size();
        meshData.vertices.resize(offset + mesh->mNumVertices * 5); // 3 (pos) + 2 (uv)

        for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
            meshData.vertices[offset + j * 5 + 0] = mesh->mVertices[j].x;
            meshData.vertices[offset + j * 5 + 1] = mesh->mVertices[j].y;
            meshData.vertices[offset + j * 5 + 2] = mesh->mVertices[j].z;

            if (mesh->HasTextureCoords(0)) {
                meshData.vertices[offset + j * 5 + 3] = mesh->mTextureCoords[0][j].x;
                meshData.vertices[offset + j * 5 + 4] = mesh->mTextureCoords[0][j].y;
            } else {
                meshData.vertices[offset + j * 5 + 3] = 0.0f;
                meshData.vertices[offset + j * 5 + 4] = 0.0f;
            }
        }
        
        unsigned int indexOffset = offset / 5;
        for (unsigned int j = 0; j < mesh->mNumFaces; ++j) {
            const aiFace& face = mesh->mFaces[j];
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                meshData.indices.push_back(face.mIndices[k] + indexOffset);
            }
        }
    }
    
    const aiMesh* mesh = scene->mMeshes[0];
    model.material = loadMaterial(scene, mesh, textureDirectory);

    glGenBuffers(1, &model.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, model.vbo);
    glBufferData(GL_ARRAY_BUFFER, meshData.vertices.size() * sizeof(float), meshData.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &model.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, meshData.indices.size() * sizeof(unsigned int), meshData.indices.data(), GL_STATIC_DRAW);

    model.indexCount = meshData.indices.size();
    return model;
}

// --- Inicjalizacja (bez zmian) ---
bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("Minimal OBJ Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
    if (!window) return false;

    glContext = SDL_GL_CreateContext(window);
    if (!glContext) return false;

    glViewport(0, 0, 640, 480);
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    if (!(IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG))) return false;

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    harpyModel = loadModel("asserts/Harpy.fbx", "asserts");
    return harpyModel.indexCount > 0;
}

// --- Render (bez zmian) ---
void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    harpyModel.render(program, rotX, rotY);
    SDL_GL_SwapWindow(window);
}

void loop() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) emscripten_cancel_main_loop();
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true; lastX = e.button.x; lastY = e.button.y;
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = false;
        } else if (e.type == SDL_MOUSEMOTION && mouseDown) {
            rotY += (e.motion.x - lastX) * 0.01f;
            rotX += (e.motion.y - lastY) * 0.01f;
            lastX = e.motion.x; lastY = e.motion.y;
        }
    }
    render();
}

// --- Główna funkcja (bez zmian) ---
int main() {
    if (!init()) {
        printf("Initialization failed.\n");
        return 1;
    }

    emscripten_set_main_loop(loop, 0, 1);

    harpyModel.cleanup();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    IMG_Quit();

    return 0;
}    
        
