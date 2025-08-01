#include <SDL.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <vector>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <stdio.h>

struct Material {
    std::string texPath;
};

struct Mesh {
    std::vector<float> vertices; // pos(3), uv(2), normal(3)
    std::vector<unsigned int> indices;
};

SDL_Window* window;
SDL_GLContext glContext;
Mesh mesh;
GLuint program, vbo, ibo, tex = 0;
std::unordered_map<std::string, Material> materials;

float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;

const char* vs = R"(
attribute vec3 aPos;
attribute vec2 aUV;
attribute vec3 aNormal;

varying vec2 vUV;
varying vec3 vNormal;

uniform float rotX;
uniform float rotY;

void main() {
    float cx = cos(rotX), sx = sin(rotX);
    float cy = cos(rotY), sy = sin(rotY);
    mat3 Rx = mat3(1, 0, 0, 0, cx, -sx, 0, sx, cx);
    mat3 Ry = mat3(cy, 0, sy, 0, 1, 0, -sy, 0, cy);
    vec3 p = Ry * Rx * aPos;
    gl_Position = vec4(p * 0.5 + vec3(0.0, 0.0, -1.0), 1.0);

    vUV = aUV;
    vNormal = normalize(Ry * Rx * aNormal);
}
)";

const char* fs = R"(
precision mediump float;

varying vec2 vUV;
varying vec3 vNormal;
uniform sampler2D tex;

void main() {
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.75));
float diff = 1.0;
    //float diff = max(dot(vNormal, lightDir), 0.0);

    vec4 texColor = texture2D(tex, vUV);
    vec3 color = texColor.rgb * diff;

    gl_FragColor = vec4(color, texColor.a);
}
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        printf("Shader compilation failed: %s\n", infoLog);
        return 0;
    }
    return shader;
}

Mesh loadMeshFromAssimp(const std::string& path, std::unordered_map<std::string, Material>& materialsOut, const std::string& basePath) {
    Mesh mesh;
    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals | aiProcess_FlipUVs);

    if (!scene || !scene->HasMeshes()) {
        printf("Assimp error: %s\n", importer.GetErrorString());
        return mesh;
    }

    const aiMesh* m = scene->mMeshes[0];

    for (unsigned int i = 0; i < m->mNumVertices; ++i) {
        aiVector3D pos = m->mVertices[i];
        aiVector3D uv = m->HasTextureCoords(0) ? m->mTextureCoords[0][i] : aiVector3D(0, 0, 0);
        aiVector3D norm = m->mNormals[i];

        mesh.vertices.push_back(pos.x);
        mesh.vertices.push_back(pos.y);
        mesh.vertices.push_back(pos.z);

        mesh.vertices.push_back(uv.x);
        mesh.vertices.push_back(uv.y);

        mesh.vertices.push_back(norm.x);
        mesh.vertices.push_back(norm.y);
        mesh.vertices.push_back(norm.z);
    }

    for (unsigned int i = 0; i < m->mNumFaces; ++i) {
        aiFace& face = m->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j)
            mesh.indices.push_back(face.mIndices[j]);
    }
/*
    // Normalize model (center & scale)
    float minX=1e10f, maxX=-1e10f, minY=1e10f, maxY=-1e10f, minZ=1e10f, maxZ=-1e10f;
    for (size_t i = 0; i < mesh.vertices.size(); i += 8) {
        float x = mesh.vertices[i + 0];
        float y = mesh.vertices[i + 1];
        float z = mesh.vertices[i + 2];
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
    }
    float cx = (minX + maxX) / 2.f;
    float cy = (minY + maxY) / 2.f;
    float cz = (minZ + maxZ) / 2.f;
    float scale = 1.f / std::max({maxX - minX, maxY - minY, maxZ - minZ});
    for (size_t i = 0; i < mesh.vertices.size(); i += 8) {
        mesh.vertices[i + 0] = (mesh.vertices[i + 0] - cx) * scale;
        mesh.vertices[i + 1] = (mesh.vertices[i + 1] - cy) * scale;
        mesh.vertices[i + 2] = (mesh.vertices[i + 2] - cz) * scale;
    }

    // Load material texture path (only first diffuse texture)
    if (scene->HasMaterials()) {
        aiMaterial* mat = scene->mMaterials[m->mMaterialIndex];
        aiString texPath;
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            Material material;
            material.texPath = std::string(texPath.C_Str());
            materialsOut["default"] = material;
        }
    }
*/
    return mesh;
}

bool init() {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("Assimp + SDL Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_OPENGL);
    glContext = SDL_GL_CreateContext(window);
    glViewport(0, 0, 800, 600);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    mesh = loadMeshFromAssimp("asserts/Harpy.fbx", materials, "asserts/");
    printf("Loaded mesh: verts=%zu, indices=%zu\n", mesh.vertices.size() / 8, mesh.indices.size());

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);

    // Bind attrib locations before linking
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aUV");
    glBindAttribLocation(program, 2, "aNormal");

    glLinkProgram(program);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(unsigned int), mesh.indices.data(), GL_STATIC_DRAW);

    // Load texture from material
    if (!materials.empty()) {
        Material& mat = materials.begin()->second;
        std::string fullTexPath = "asserts/" + mat.texPath;
        int w, h, comp;
        unsigned char* data = stbi_load(fullTexPath.c_str(), &w, &h, &comp, 4);
        if (!data) {
            printf("Failed to load texture: %s\n", fullTexPath.c_str());
            return false;
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        stbi_image_free(data);
    } else {
        printf("No materials found, no texture loaded.\n");
    }

    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);

    return true;
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(program);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));

    glUniform1f(glGetUniformLocation(program, "rotX"), rotX);
    glUniform1f(glGetUniformLocation(program, "rotY"), rotY);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glDrawElements(GL_TRIANGLES, mesh.indices.size(), GL_UNSIGNED_INT, 0);

    SDL_GL_SwapWindow(window);
}

void loop() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) emscripten_cancel_main_loop();
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            lastX = e.button.x;
            lastY = e.button.y;
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = false;
        } else if (e.type == SDL_MOUSEMOTION && mouseDown) {
            rotY += (e.motion.x - lastX) * 0.01f;
            rotX += (e.motion.y - lastY) * 0.01f;
            lastX = e.motion.x;
            lastY = e.motion.y;
        }
    }
    render();
}

int main() {
    if (!init()) {
        printf("Initialization failed!\n");
        return 1;
    }
    emscripten_set_main_loop(loop, 0, 1);
    return 0;
}
