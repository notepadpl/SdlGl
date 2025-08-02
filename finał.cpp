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
struct Vertex {
    float x, y, z;
    float u, v;
    float nx, ny, nz;
};

struct Material {
    std::string texPathDiffuse;
    std::string texPathNormal;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<Material> materials;
    std::vector<int> materialIndices; // indeks materiału przypisany do każdej twarzy
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
//gl_Position = vec4(p + vec3(0.0, 0.0, -2.0), 1.0); // dalej odsunięte, jeśli nadal nie mieści się
gl_Position = vec4(p + vec3(0.0, 0.0, -0.5), 1.0);

 //   gl_Position = vec4(p * 0.5 + vec3(0.0, 0.0, -3.0), 1.0);

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
//float diff = 1.0;
    float diff = max(dot(vNormal, lightDir), 0.0);

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
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_FlipUVs);

    if (!scene || !scene->HasMeshes()) {
        std::cerr << "Assimp error: " << importer.GetErrorString() << "\n";
        return mesh;
    }

    const float scale = 0.02f;
    unsigned int vertexOffset = 0;

    for (unsigned int mIndex = 0; mIndex < scene->mNumMeshes; ++mIndex) {
        const aiMesh* m = scene->mMeshes[mIndex];
        int materialIndex = m->mMaterialIndex;

        for (unsigned int i = 0; i < m->mNumVertices; ++i) {
            Vertex v;

            aiVector3D pos = m->mVertices[i];
            v.x = pos.x * scale;
            v.y = pos.y * scale;
            v.z = pos.z * scale;

            if (m->HasTextureCoords(0)) {
                v.u = m->mTextureCoords[0][i].x;
                v.v = m->mTextureCoords[0][i].y;
            } else {
                v.u = v.v = 0.0f;
                std::cerr << "Warning: Missing UVs in mesh " << mIndex << "\n";
            }

            if (m->HasNormals()) {
                aiVector3D norm = m->mNormals[i];
                v.nx = norm.x;
                v.ny = norm.y;
                v.nz = norm.z;
            } else {
                v.nx = 0;
                v.ny = 1;
                v.nz = 0;
                std::cerr << "Warning: Missing normals in mesh " << mIndex << "\n";
            }

            mesh.vertices.push_back(v);
        }

        for (unsigned int f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            if (face.mNumIndices != 3) {
                std::cerr << "Warning: Non-triangle face detected.\n";
                continue;
            }

            for (unsigned int j = 0; j < 3; ++j) {
                mesh.indices.push_back(vertexOffset + face.mIndices[j]);
            }

            mesh.materialIndices.push_back(materialIndex);
        }

        vertexOffset += m->mNumVertices;
    }

    // Wczytaj materiały
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        Material mtl;

        aiString texPath;

        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            mtl.texPathDiffuse = texPath.C_Str();
            materialsOut[mtl.texPathDiffuse] = mtl;
        }

        if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS) {
            mtl.texPathNormal = texPath.C_Str();
            materialsOut[mtl.texPathNormal] = mtl;
        }

        mesh.materials.push_back(mtl);
    }

    return mesh;
}
/*
Mesh loadMeshFromAssimp(const std::string& path, std::unordered_map<std::string, Material>& materialsOut, const std::string& basePath) {
    Mesh mesh;
    Assimp::Importer importer;

    /*Używamy PreTransformVertices i ConvertToLeftHanded
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_PreTransformVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_FlipUVs);*/
const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals);

    
    if (!scene || !scene->HasMeshes()) {
        printf("Assimp error: %s\n", importer.GetErrorString());
        return mesh;
    }

    const float scale = 0.02f;  // Skalowanie modelu (opcjonalne)

    unsigned int vertexOffset = 0;

    for (unsigned int mIndex = 0; mIndex < scene->mNumMeshes; ++mIndex) {
        const aiMesh* m = scene->mMeshes[mIndex];

        // Wczytaj wierzchołki
        for (unsigned int i = 0; i < m->mNumVertices; ++i) {
            aiVector3D pos = m->mVertices[i];
            aiVector3D uv = m->HasTextureCoords(0) ? m->mTextureCoords[0][i] : aiVector3D(0, 0, 0);
            aiVector3D norm = m->HasNormals() ? m->mNormals[i] : aiVector3D(0, 1, 0);

            // Skalujemy pozycję, reszta bez zmian
            mesh.vertices.push_back(pos.x * scale);
            mesh.vertices.push_back(pos.y * scale);
            mesh.vertices.push_back(pos.z * scale);

            mesh.vertices.push_back(uv.x);
            mesh.vertices.push_back(uv.y);

            mesh.vertices.push_back(norm.x);
            mesh.vertices.push_back(norm.y);
            mesh.vertices.push_back(norm.z);
        }

        // Wczytaj indeksy (uwzględniając przesunięcie)
        for (unsigned int f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                mesh.indices.push_back(vertexOffset + face.mIndices[j]);
            }
        }

        vertexOffset += m->mNumVertices;
    }

    // Wczytaj materiały (osobno, po meshach)
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        aiString texPath;
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            std::string textureFile = texPath.C_Str();
            materialsOut[textureFile].texPath = textureFile;
        }
    }

    return mesh;
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

    unsigned int vertexOffset = 0;

    for (unsigned int mIndex = 0; mIndex < scene->mNumMeshes; ++mIndex) {
        const aiMesh* m = scene->mMeshes[mIndex];

        // Wczytaj wierzchołki
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

        // Wczytaj indeksy (z offsetem, bo łączymy siatki)
        for (unsigned int f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                mesh.indices.push_back(vertexOffset + face.mIndices[j]);
            }
        }

        vertexOffset += m->mNumVertices;

        // Możesz tu też wczytać materiały dla każdego mesha, jeśli chcesz
        // Wczytaj materiały
for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
    aiMaterial* mat = scene->mMaterials[i];
    aiString texPath;
    if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
        std::string textureFile = texPath.C_Str();
        materials[textureFile].texPath = textureFile;
    }
}
        
    }

    // Normalizacja modelu — to możesz przenieść z Twojej oryginalnej funkcji
    // ...

    // Materiały możesz wyciągnąć podobnie, np. z scene->mMaterials i powiązać z meshami

    return mesh;
}

*/

bool init() {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("Assimp + SDL Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_OPENGL);
    glContext = SDL_GL_CreateContext(window);
    glViewport(0, 0, 800, 600);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.9f, 0.1f, 0.1f, 1.0f);

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
