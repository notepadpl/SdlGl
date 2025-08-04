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
#include <string>

// Globalne zmienne, bez zmian
SDL_Window* window;
SDL_GLContext glContext;
float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;
GLuint program;

// --- Struktury i klasy ---
struct MeshData {
    std::vector<float> vertices; // Pozycja(3), Normalna(3), UV(2)
    std::vector<unsigned int> indices;
};

// Struktura materiału - bez zmian
struct Material {
    GLuint diffuse = 0;
    GLuint specular = 0;
    GLuint normal = 0;
    GLuint emissive = 0;
};

class Model {
public:
    GLuint vbo, ibo;
    size_t indexCount;
    Material material;

    void render(GLuint program, float rotX, float rotY) {
        glUseProgram(program);

        GLint rotXLoc = glGetUniformLocation(program, "rotX");
        GLint rotYLoc = glGetUniformLocation(program, "rotY");
        glUniform1f(rotXLoc, rotX);
        glUniform1f(rotYLoc, rotY);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, material.diffuse);
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
    void cleanup() {
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ibo);
        glDeleteTextures(1, &material.diffuse);
        glDeleteTextures(1, &material.specular);
        glDeleteTextures(1, &material.normal);
        glDeleteTextures(1, &material.emissive);
    }
};

Model harpyModel;

// --- Funkcje pomocnicze ---
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

// Funkcja ładowania pojedynczej tekstury z materiału Assimp
GLuint loadTextureFromMaterial(aiMaterial* mat, aiTextureType type, const std::string& directory) {
    if (mat->GetTextureCount(type) > 0) {
        aiString path;
        mat->GetTexture(type, 0, &path);

        // Sklejamy katalog z nazwą pliku z Assimp
        std::string fullPath = directory + "/" + std::string(path.C_Str());
        
        SDL_Surface* surface = IMG_Load(fullPath.c_str());
        if (!surface) {
            printf("Nie udalo sie zaladowac tekstury: %s\n", fullPath.c_str());
            return 0;
        }
        printf("Zaladowano teksture: %s\n", fullPath.c_str());

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

// Funkcja ładowania wszystkich tekstur dla danego materiału
Material loadMaterial(const aiScene* scene, const aiMesh* mesh, const std::string& directory) {
    Material mat;
    if (!scene->HasMaterials()) return mat;
    
    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

    mat.diffuse  = loadTextureFromMaterial(material, aiTextureType_DIFFUSE, directory);
    mat.specular = loadTextureFromMaterial(material, aiTextureType_SPECULAR, directory);
    mat.normal   = loadTextureFromMaterial(material, aiTextureType_NORMALS, directory);
    mat.emissive = loadTextureFromMaterial(material, aiTextureType_EMISSIVE, directory);

    return mat;
}

// Funkcja do wczytywania geometrii
MeshData loadMeshFromAssimp(const char* path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_CalcTangentSpace);
    MeshData meshData;

    if (!scene || !scene->HasMeshes()) {
        printf("Failed to load mesh: %s\n", importer.GetErrorString());
        return meshData;
    }
    printf("Model loaded successfully. Found %d meshes.\n", scene->mNumMeshes);
    // ... reszta kodu do wczytywania wierzchołków i indeksów bez zmian ...
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


// --- Nowa wersja funkcji loadModel, która łączy ładowanie geometrii i materiałów
Model loadModel(const char* meshPath, const std::string& textureDirectory) {
    Model newModel;
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        meshPath, 
        aiProcess_Triangulate | 
        aiProcess_JoinIdenticalVertices | 
        aiProcess_GenNormals | 
        aiProcess_CalcTangentSpace
    );

    if (!scene || !scene->HasMeshes()) {
        printf("Nie udalo sie zaladowac modelu: %s\n", importer.GetErrorString());
        return Model();
    }
    
    // Wczytujemy dane siatki za pomocą istniejącej funkcji
    MeshData meshData;
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
    
    // Wczytujemy materiał
    const aiMesh* mesh = scene->mMeshes[0];
    newModel.material = loadMaterial(scene, mesh, textureDirectory);

    glGenBuffers(1, &newModel.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, newModel.vbo);
    glBufferData(GL_ARRAY_BUFFER, meshData.vertices.size() * sizeof(float), meshData.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &newModel.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newModel.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, meshData.indices.size() * sizeof(unsigned int), meshData.indices.data(), GL_STATIC_DRAW);
    
    newModel.indexCount = meshData.indices.size();

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    return newModel;
}

// --- Fragment shader z poprawionym łączeniem tekstur ---
const char* fs = R"(
precision mediump float;

uniform sampler2D tex;
uniform sampler2D specularMap;
uniform sampler2D normalMap;
uniform sampler2D emissiveMap;

varying vec2 vUV;

void main() {
    vec4 diffuseColor  = texture2D(tex, vUV);
    // Możesz łączyć kolory tekstur, np.:
    // vec4 specularColor = texture2D(specularMap, vUV);
    // gl_FragColor = diffuseColor * (1.0 - specularColor);
    
    gl_FragColor = diffuseColor; 
}
)";

// --- Główny kod programu (poprawiony) ---
bool init() {
    // ... inicjalizacja SDL/OpenGL bez zmian ...
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

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    // --- Ładowanie modelu ---
    harpyModel = loadModel("asserts/Harpy.fbx", "asserts");
    if(harpyModel.indexCount == 0) {
        printf("Failed to load Harpy model.\n");
        return false;
    }

    return true;
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
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
