#include <SDL.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <cmath>
#include <stdio.h>
#include <SDL_image.h>
#include <unistd.h>
// Globalne zmienne, ale teraz uproszczone
SDL_Window* window;
SDL_GLContext glContext;
float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;
GLuint program;

// --- Nowa struktura dla modelu ---
struct MeshData {
    std::vector<float> vertices; // Pozycja(3), Normalna(3), UV(2)
    std::vector<unsigned int> indices;
};
//tekstury w assimp
struct Material {
    GLuint diffuse = 0;
    GLuint specular = 0;
    GLuint normal = 0;
    GLuint emissive = 0;
};
class Model {
public:
    GLuint vbo, ibo, textureID;
    size_t indexCount;
Material material;
    // Metoda renderująca model
    void render(GLuint program, float rotX, float rotY) {
        glUseProgram(program);

        // Ustawienie uniformów rotacji
        GLint rotXLoc = glGetUniformLocation(program, "rotX");
        GLint rotYLoc = glGetUniformLocation(program, "rotY");
        glUniform1f(rotXLoc, rotX);
        glUniform1f(rotYLoc, rotY);
        
        // Wiązanie tekstury
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glUniform1i(glGetUniformLocation(program, "tex"), 0);

        // Wiązanie buforów
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

        // Konfiguracja atrybutów wierzchołków
        GLint posLoc = glGetAttribLocation(program, "aPos");
        GLint normalLoc = glGetAttribLocation(program, "aNormal");
        GLint uvLoc = glGetAttribLocation(program, "aUV");
        
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)0);
        
        glEnableVertexAttribArray(normalLoc);
        glVertexAttribPointer(normalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 3));
        
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 6));
        
        // Rysowanie elementów z bufora indeksów
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, (void*)0);
    }

    // Metoda zwalniająca zasoby
    void cleanup() {
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ibo);
        glDeleteTextures(1, &textureID);
    }
};

Model harpyModel; // Instancja naszego modelu

// --- Funkcje pomocnicze ---
GLuint loadTextureFromMaterial(aiMaterial* mat, aiTextureType type) {
    if (mat->GetTextureCount(type) > 0) {
        aiString path;
        mat->GetTexture(type, 0, &path);

        std::string fullPath = std::string("asserts/") + path.C_Str();
        SDL_Surface* surface = IMG_Load(fullPath.c_str());
        if (!surface) {
            printf("Nie udalo sie zaladowac: %s\n", fullPath.c_str());
            return 0;
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
Material loadMaterial(const aiScene* scene, const aiMesh* mesh) {
    Material mat;
    if (!scene->HasMaterials()) return mat;

    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

    mat.diffuse  = loadTextureFromMaterial(material, aiTextureType_DIFFUSE);
    mat.specular = loadTextureFromMaterial(material, aiTextureType_SPECULAR);
    mat.normal   = loadTextureFromMaterial(material, aiTextureType_NORMALS);
    mat.emissive = loadTextureFromMaterial(material, aiTextureType_EMISSIVE);

    return mat;
}
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

MeshData loadMeshFromAssimp(const char* path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
    MeshData meshData;

    if (!scene || !scene->HasMeshes()) {
        printf("Failed to load: %s\n", importer.GetErrorString());
        return meshData;
    }
    printf("Model loaded successfully. Found %d meshes.\n", scene->mNumMeshes);

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        unsigned int currentVerticesSize = meshData.vertices.size();
        meshData.vertices.resize(currentVerticesSize + mesh->mNumVertices * 8);

        for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
            meshData.vertices[currentVerticesSize + j * 8 + 0] = mesh->mVertices[j].x;
            meshData.vertices[currentVerticesSize + j * 8 + 1] = mesh->mVertices[j].y;
            meshData.vertices[currentVerticesSize + j * 8 + 2] = mesh->mVertices[j].z;
            
            if (mesh->HasNormals()){
                meshData.vertices[currentVerticesSize + j * 8 + 3] = mesh->mNormals[j].x;
                meshData.vertices[currentVerticesSize + j * 8 + 4] = mesh->mNormals[j].y;
                meshData.vertices[currentVerticesSize + j * 8 + 5] = mesh->mNormals[j].z;
            } else {
                meshData.vertices[currentVerticesSize + j * 8 + 3] = 0.0f; meshData.vertices[currentVerticesSize + j * 8 + 4] = 0.0f; meshData.vertices[currentVerticesSize + j * 8 + 5] = 0.0f;
            }

            if (mesh->HasTextureCoords(0)){
                meshData.vertices[currentVerticesSize + j * 8 + 6] = mesh->mTextureCoords[0][j].x;
                meshData.vertices[currentVerticesSize + j * 8 + 7] = mesh->mTextureCoords[0][j].y;
            } else {
                meshData.vertices[currentVerticesSize + j * 8 + 6] = 0.0f; meshData.vertices[currentVerticesSize + j * 8 + 7] = 0.0f;
            }
        }

        unsigned int indexOffset = currentVerticesSize / 8;
        for (unsigned int j = 0; j < mesh->mNumFaces; ++j) {
            const aiFace& face = mesh->mFaces[j];
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                meshData.indices.push_back(face.mIndices[k] + indexOffset);
            }
        }
    }
    printf("Total Vertices: %zu, Total Indices: %zu\n", meshData.vertices.size() / 8, meshData.indices.size());
    return meshData;
}

GLuint loadTexture(const char* path) {
    SDL_Surface* surface = IMG_Load(path);
    if (!surface) {
        printf("Nie udalo sie zaladowac obrazu! SDL_image Error: %s\n", IMG_GetError());
        return 0;
    }
    printf("Tekstura zaladowana pomyslnie: %s\n", path);

    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    GLenum format;
    if (surface->format->BytesPerPixel == 4) {
        format = GL_RGBA;
    } else {
        format = GL_RGB;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, format, surface->w, surface->h, 0, format, GL_UNSIGNED_BYTE, surface->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    SDL_FreeSurface(surface);
    return texID;
}

Model loadModel(const char* meshPath, const char* texturePath) {
    Model newModel;
    MeshData meshData = loadMeshFromAssimp(meshPath);
    if(meshData.indices.empty()) {
        printf("Error: Model data is empty.\n");
        return newModel;
    }

   // newModel.textureID = loadTexture(texturePath);
Assimp::Importer importer;
const aiScene* scene = importer.ReadFile(
    meshPath, 
    aiProcess_Triangulate | 
    aiProcess_JoinIdenticalVertices | 
    aiProcess_GenNormals | 
    aiProcess_CalcTangentSpace
);

if (!scene || !scene->HasMeshes()) {
    printf("Nie udało się załadować modelu: %s\n", importer.GetErrorString());
    return Model(); // lub pusty Model
}

const aiMesh* mesh = scene->mMeshes[0];
newModel.material = loadMaterial(scene, mesh);

    glGenBuffers(1, &newModel.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, newModel.vbo);
    glBufferData(GL_ARRAY_BUFFER, meshData.vertices.size() * sizeof(float), meshData.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &newModel.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newModel.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, meshData.indices.size() * sizeof(unsigned int), meshData.indices.data(), GL_STATIC_DRAW);
    
    newModel.indexCount = meshData.indices.size();

    // Odwiązanie buforów, aby uniknąć problemów w innych częściach kodu
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    return newModel;
}

// --- Shader code (bez zmian) ---
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
uniform sampler2D specularMap;
uniform sampler2D normalMap;
uniform sampler2D emissiveMap;

varying vec2 vUV;

void main() {
    vec4 diffuseColor  = texture2D(tex, vUV);
    vec4 specularColor = texture2D(specularMap, vUV);
    vec4 normalColor   = texture2D(normalMap, vUV);
    vec4 emissiveColor = texture2D(emissiveMap, vUV);

    vec4 finalColor = diffuseColor + 0.3 * specularColor + 0.1 * emissiveColor;
    gl_FragColor = finalColor;
}
)";


bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    window = SDL_CreateWindow("OBJ Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
    if (!window) {
        printf("SDL_CreateWindow Error: %s\n", SDL_GetError());
        return false;
    }
    glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        printf("SDL_GL_CreateContext Error: %s\n", SDL_GetError());
        return false;
    }
    glViewport(0, 0, 640, 480);
    glClearColor(0.1f, 0.9f, 0.1f, 1.0f);

    int imgFlags = IMG_INIT_JPG | IMG_INIT_PNG;
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        printf("SDL_image nie moglo sie zainicjalizowac! SDL_image Error: %s\n", IMG_GetError());
        return false;
    }

    // Ładowanie shaderów
    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    // --- Ładowanie modelu za pomocą nowej funkcji ---
    harpyModel = loadModel("asserts/Harpy.fbx", "asserts/Hair.png");
    if(harpyModel.indexCount == 0) {
        printf("Failed to load Harpy model.\n");
        return false;
    }

    return true;
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D, this->material.diffuse); // lub po prostu this->material...
glUniform1i(glGetUniformLocation(program, "tex"), 0);

glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, material.specular);
glUniform1i(glGetUniformLocation(program, "specularMap"), 1);

glActiveTexture(GL_TEXTURE2);
glBindTexture(GL_TEXTURE_2D, material.normal);
glUniform1i(glGetUniformLocation(program, "normalMap"), 2);

glActiveTexture(GL_TEXTURE3);
glBindTexture(GL_TEXTURE_2D, material.emissive);
glUniform1i(glGetUniformLocation(program, "emissiveMap"), 3);

    // --- Renderowanie modelu za pomocą nowej metody ---
    harpyModel.render(program, rotX, rotY);
    
    SDL_GL_SwapWindow(window);
}

void loop(){
    SDL_Event e;
    while(SDL_PollEvent(&e)){
        if (e.type == SDL_QUIT) emscripten_cancel_main_loop();
        else if(e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT){
            mouseDown = true; lastX = e.button.x; lastY = e.button.y;
        } else if(e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT){
            mouseDown = false;
        } else if(e.type == SDL_MOUSEMOTION && mouseDown){
            rotY += (e.motion.x - lastX) * 0.01f;
            rotX += (e.motion.y - lastY) * 0.01f;
            lastX = e.motion.x; lastY = e.motion.y;
        }
    }
    render();
}

int main() {
 //   system("ls asserts > texture.txt");
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
