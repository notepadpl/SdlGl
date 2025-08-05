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
#include <iostream>

// --- Globalne zmienne ---
SDL_Window* window = nullptr;
SDL_GLContext glContext = nullptr;
GLuint program = 0;
float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;

// --- Shadery ---
const char* vs = R"(
attribute vec3 aPos;
attribute vec3 aNormal;
attribute vec2 aUV;

varying vec3 vNormal;
varying vec2 vUV;

uniform float rotX, rotY;

void main(){
    float cx = cos(rotX), sx = sin(rotX);
    float cy = cos(rotY), sy = sin(rotY);
    mat3 Rx = mat3(1, 0, 0, 0, cx, -sx, 0, sx, cx);
    mat3 Ry = mat3(cy, 0, sy, 0, 1, 0, -sy, 0, cy);
    vec3 p = Ry * Rx * aPos;
    gl_Position = vec4(p * 0.1, 1.0);
    
    vNormal = normalize(Ry * Rx * aNormal);
    vUV = aUV;
}
)";

const char* fs = R"(
precision mediump float;

uniform sampler2D tex;
varying vec2 vUV;
varying vec3 vNormal;

void main() {
    vec3 texColor = texture2D(tex, vUV).rgb;

    // Światło – niech podkreśla teksturę
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff = max(dot(normalize(vNormal), lightDir), 0.0);

    // Tekstura z lekkim światłem – nie za jasno, żeby model był przyciemniony
    vec3 color = texColor * (0.4 + 0.6 * diff); // 0.4 bazowe + światło

    gl_FragColor = vec4(color, 1.0);
}

)";

// --- Deklaracje klas ---
struct Material {
    GLuint diffuse = 0;
    GLuint specular = 0;
    GLuint normal = 0;
    GLuint emissive = 0;

    void bind(GLuint program) const;
    void cleanup();
};

class Mesh {
public:
    GLuint vbo = 0, ibo = 0;
    size_t indexCount = 0;
    Material material;

    void render(GLuint program);
    void cleanup();
};

class Model {
public:
    std::vector<Mesh> meshes;

    Model() = default;
    void load(const std::string& modelPath, const std::string& textureDir);
    void render(GLuint program, float rotX, float rotY);
    void cleanup();
};

// --- Implementacja klas ---
void Material::bind(GLuint program) const {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, diffuse);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, specular);
    glUniform1i(glGetUniformLocation(program, "specularMap"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, normal);
    glUniform1i(glGetUniformLocation(program, "normalMap"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, emissive);
    glUniform1i(glGetUniformLocation(program, "emissiveMap"), 3);
}

void Material::cleanup() {
    if (diffuse) glDeleteTextures(1, &diffuse);
    if (specular) glDeleteTextures(1, &specular);
    if (normal) glDeleteTextures(1, &normal);
    if (emissive) glDeleteTextures(1, &emissive);
}

void Mesh::render(GLuint program) {
    material.bind(program);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    GLint posLoc = glGetAttribLocation(program, "aPos");
    GLint normalLoc = glGetAttribLocation(program, "aNormal");
    GLint uvLoc = glGetAttribLocation(program, "aUV");

    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)0);

    glEnableVertexAttribArray(normalLoc);
    glVertexAttribPointer(normalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 3));

    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 6));

    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, (void*)0);
}

void Mesh::cleanup() {
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    material.cleanup();
}

// --- Funkcje pomocnicze do ładowania zasobów ---
GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Shader compilation error: " << infoLog << "\n";
    }
    return shader;
}

GLuint loadTextureFromMaterial(aiMaterial* mat, aiTextureType type, const std::string& directory) {
    if (mat->GetTextureCount(type) > 0) {
        aiString path;
        mat->GetTexture(type, 0, &path);
        std::string fullPath = directory + "/Hair.png"; // Wczytujemy "na sztywno"
        
        std::cout << "Proba zaladowania tekstury: " << fullPath << "\n";

        
       // std::string fullPath = directory + "/" + std::string(path.C_Str());
        std::cout << "Proba zaladowania tekstury: " << fullPath << "\n";

        SDL_Surface* surface = IMG_Load(fullPath.c_str());
        if (!surface) {
            std::cerr << "Nie udalo sie zaladowac tekstury: " << fullPath << ", SDL_image Error: " << IMG_GetError() << "\n";
            return 0;
        }
        std::cout << "Tekstura zaladowana: " << fullPath << "\n";

        GLuint texID;
        glGenTextures(1, &texID);
        glBindTexture(GL_TEXTURE_2D, texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GLenum format = (surface->format->BytesPerPixel == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, surface->w, surface->h, 0, format, GL_UNSIGNED_BYTE, surface->pixels);
        //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, surface->w, surface->h, 0, GL_RGB, GL_UNSIGNED_BYTE, surface->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
        
       // glGenerateMipmap(GL_TEXTURE_2D);

        SDL_FreeSurface(surface);
        return texID;
    }
    return 0;
}

Material loadMaterial(const aiScene* scene, const aiMesh* mesh, const std::string& directory) {
    Material mat;
    if (!scene->HasMaterials()) {
        std::cerr << "Brak materialow w scenie.\n";
        return mat;
    }

    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    mat.diffuse = loadTextureFromMaterial(material, aiTextureType_DIFFUSE, directory);
    mat.specular = loadTextureFromMaterial(material, aiTextureType_SPECULAR, directory);
    mat.normal = loadTextureFromMaterial(material, aiTextureType_NORMALS, directory);
    mat.emissive = loadTextureFromMaterial(material, aiTextureType_EMISSIVE, directory);

    return mat;
}

// --- Model (implementacja metod) ---
void Model::load(const std::string& path, const std::string& textureDir) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

    if (!scene || !scene->HasMeshes()) {
        std::cerr << "Nie udalo sie zaladowac modelu: " << importer.GetErrorString() << "\n";
        return;
    }

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        Mesh newMesh;

        std::vector<float> vertices;
        std::vector<unsigned int> indices;

        for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
            vertices.push_back(mesh->mVertices[j].x);
            vertices.push_back(mesh->mVertices[j].y);
            vertices.push_back(mesh->mVertices[j].z);

            if (mesh->HasNormals()) {
                vertices.push_back(mesh->mNormals[j].x);
                vertices.push_back(mesh->mNormals[j].y);
                vertices.push_back(mesh->mNormals[j].z);
            } else {
                vertices.insert(vertices.end(), {0.0f, 0.0f, 0.0f});
            }

            if (mesh->HasTextureCoords(0)) {
                vertices.push_back(mesh->mTextureCoords[0][j].x);
                vertices.push_back(mesh->mTextureCoords[0][j].y);
            } else {
                vertices.insert(vertices.end(), {0.0f, 0.0f});
            }
        }

        for (unsigned int j = 0; j < mesh->mNumFaces; ++j) {
            const aiFace& face = mesh->mFaces[j];
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                indices.push_back(face.mIndices[k]);
            }
        }

        glGenBuffers(1, &newMesh.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, newMesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

        glGenBuffers(1, &newMesh.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newMesh.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        newMesh.indexCount = indices.size();
        newMesh.material = loadMaterial(scene, mesh, textureDir);
        meshes.push_back(newMesh);
    }
}

void Model::render(GLuint program, float rotX, float rotY) {
    glUseProgram(program);

    GLint rotXLoc = glGetUniformLocation(program, "rotX");
    GLint rotYLoc = glGetUniformLocation(program, "rotY");
    glUniform1f(rotXLoc, rotX);
    glUniform1f(rotYLoc, rotY);

    for (auto& mesh : meshes) {
        mesh.render(program);
    }
}

void Model::cleanup() {
    for (auto& mesh : meshes) {
        mesh.cleanup();
    }
    meshes.clear();
}

// Deklaracja globalnego obiektu modelu
Model harpyModel;

// --- Funkcje główne programu ---
bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("Model Loader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
    if (!window) {
        std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << "\n";
        return false;
    }
    glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "SDL_GL_CreateContext Error: " << SDL_GetError() << "\n";
        return false;
    }

    glViewport(0, 0, 640, 480);
    glClearColor(0.2f, 0.9f, 0.2f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        std::cerr << "SDL_image Error: " << IMG_GetError() << "\n";
        return false;
    }

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    return true;
}

void cleanup() {
    harpyModel.cleanup();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    IMG_Quit();
}

// Funkcja renderująca, która zostanie wywołana w pętli głównej
void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    harpyModel.render(program, rotX, rotY);
    SDL_GL_SwapWindow(window);
}

// Główna pętla, która będzie wywoływana przez Emscripten
void main_loop() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            emscripten_cancel_main_loop();
        } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
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
        std::cerr << "Inicjalizacja nie powiodla sie.\n";
        return 1;
    }
std::cout << "Ladowanie modelu..." << std::endl;
harpyModel.load("asserts/Harpy.fbx", "asserts");
std::cout << "Model zaladowany. Liczba meshy: " << harpyModel.meshes.size() << std::endl;

    //harpyModel.load("assets/Harpy.fbx", "assets");
    
    emscripten_set_main_loop(main_loop, 0, 1);
    
    cleanup();

    return 0;
}
