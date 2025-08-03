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

GLuint texture;
SDL_Window* window;
SDL_GLContext glContext;
float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;

struct Mesh {
    std::vector<float> vertices; // Pozycja(3), Normalna(3), UV(2) = 8 floatow
    std::vector<unsigned int> indices;
};

Mesh mesh;
GLuint program, vbo, ibo;

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
    gl_Position = vec4(p * 0.05, 1.0);
    vNormal = normalize(Ry * Rx * aNormal);
    vUV = aUV;
}
)";

// Uproszczony shader do testowania
const char* fs = R"(
precision mediump float;
uniform sampler2D tex;
varying vec2 vUV;
void main() {
    gl_FragColor = texture2D(tex, vUV);
}
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
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

    Mesh m;

    if (!scene || !scene->HasMeshes()) {
        printf("Failed to load: %s\n", importer.GetErrorString());
        return m;
    }
    printf("Model loaded successfully. Found %d meshes.\n", scene->mNumMeshes);

    // Bierzemy pod uwagę wszystkie siatki
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* meshData = scene->mMeshes[i];

        // Dodajemy wierzcholki z biezacej siatki do glownego bufora
        unsigned int currentVerticesSize = m.vertices.size();
        m.vertices.resize(currentVerticesSize + meshData->mNumVertices * 8);

        for (unsigned int j = 0; j < meshData->mNumVertices; ++j) {
            // Pozycja
            m.vertices[currentVerticesSize + j * 8 + 0] = meshData->mVertices[j].x;
            m.vertices[currentVerticesSize + j * 8 + 1] = meshData->mVertices[j].y;
            m.vertices[currentVerticesSize + j * 8 + 2] = meshData->mVertices[j].z;
            
            // Normalne
            if (meshData->HasNormals()){
                m.vertices[currentVerticesSize + j * 8 + 3] = meshData->mNormals[j].x;
                m.vertices[currentVerticesSize + j * 8 + 4] = meshData->mNormals[j].y;
                m.vertices[currentVerticesSize + j * 8 + 5] = meshData->mNormals[j].z;
            } else {
                m.vertices[currentVerticesSize + j * 8 + 3] = 0.0f; m.vertices[currentVerticesSize + j * 8 + 4] = 0.0f; m.vertices[currentVerticesSize + j * 8 + 5] = 0.0f;
            }

            // UV
            if (meshData->HasTextureCoords(0)){
                m.vertices[currentVerticesSize + j * 8 + 6] = meshData->mTextureCoords[0][j].x;
                m.vertices[currentVerticesSize + j * 8 + 7] = meshData->mTextureCoords[0][j].y;
            } else {
                m.vertices[currentVerticesSize + j * 8 + 6] = 0.0f; m.vertices[currentVerticesSize + j * 8 + 7] = 0.0f;
            }
        }

        // Dodajemy indeksy, pamietajac o offsetach
        unsigned int indexOffset = currentVerticesSize / 8;
        for (unsigned int j = 0; j < meshData->mNumFaces; ++j) {
            const aiFace& face = meshData->mFaces[j];
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                m.indices.push_back(face.mIndices[k] + indexOffset);
            }
        }
    }
    printf("Total Vertices: %zu, Total Indices: %zu\n", m.vertices.size() / 8, m.indices.size());
    return m;
}
Mesh loadMeshFromAssimp2(const char* path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

    Mesh m;
    if (!scene || !scene->HasMeshes()) {
        printf("Failed to load: %s\n", importer.GetErrorString());
        return m;
    }
    printf("Model loaded successfully.\n");
    const aiMesh* meshData = scene->mMeshes[0];

    if (meshData->mMaterialIndex >= 0) {
        const aiMaterial* material = scene->mMaterials[meshData->mMaterialIndex];
        aiString texturePath;
        if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS) {
            printf("Sciezka do tekstury: %s\n", texturePath.C_Str());
        }
    }
    if (!meshData->HasTextureCoords(0)) {
        printf("WARNING: Mesh does not have texture coordinates!\n");
    }

    m.vertices.resize(meshData->mNumVertices * 8);
    for (unsigned int i = 0; i < meshData->mNumVertices; ++i) {
        m.vertices[i * 8 + 0] = meshData->mVertices[i].x;
        m.vertices[i * 8 + 1] = meshData->mVertices[i].y;
        m.vertices[i * 8 + 2] = meshData->mVertices[i].z;

        if (meshData->HasNormals()){
            m.vertices[i * 8 + 3] = meshData->mNormals[i].x;
            m.vertices[i * 8 + 4] = meshData->mNormals[i].y;
            m.vertices[i * 8 + 5] = meshData->mNormals[i].z;
        } else {
            m.vertices[i * 8 + 3] = 0.0f; m.vertices[i * 8 + 4] = 0.0f; m.vertices[i * 8 + 5] = 0.0f;
        }

        if (meshData->HasTextureCoords(0)){
            m.vertices[i * 8 + 6] = meshData->mTextureCoords[0][i].x;
            m.vertices[i * 8 + 7] = meshData->mTextureCoords[0][i].y;
        } else {
            m.vertices[i * 8 + 6] = 0.0f; m.vertices[i * 8 + 7] = 0.0f;
        }
    }

    for (unsigned int i = 0; i < meshData->mNumFaces; ++i) {
        const aiFace& face = meshData->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            m.indices.push_back(face.mIndices[j]);
        }
    }
    printf("Vertices: %zu, Indices: %zu\n", m.vertices.size()/8, m.indices.size());
    return m;
}

bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("OBJ Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              640, 480, SDL_WINDOW_OPENGL);
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

    // inicjalizacja SDL_image
    int imgFlags = IMG_INIT_JPG | IMG_INIT_PNG;
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        printf("SDL_image nie moglo sie zainicjalizowac! SDL_image Error: %s\n", IMG_GetError());
        return false;
    }

    // Zaladowanie tekstury
    SDL_Surface* surface = IMG_Load("asserts/Hair.png");
    if (!surface) {
        printf("Nie udalo sie zaladowac obrazu! SDL_image Error: %s\n", IMG_GetError());
        return false;
    }
    printf("Tekstura zaladowana pomyslnie.\n");

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    SDL_FreeSurface(surface);
    IMG_Quit();

    // BARDZO WAŻNE: upewnij się, że plik jest preładowany poprawną ścieżką
    mesh = loadMeshFromAssimp("asserts/Harpy.fbx");
    if(mesh.indices.empty()) {
        printf("Error: Model data is empty.\n");
        return false;
    }

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    glUseProgram(program);

    // Konfiguracja atrybutow wierzcholkow
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(unsigned int), mesh.indices.data(), GL_STATIC_DRAW);

    GLint posLoc = glGetAttribLocation(program, "aPos");
    GLint normalLoc = glGetAttribLocation(program, "aNormal");
    GLint uvLoc = glGetAttribLocation(program, "aUV");
    printf("aPos location: %d, aNormal location: %d, aUV location: %d\n", posLoc, normalLoc, uvLoc);

    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)0);

    glEnableVertexAttribArray(normalLoc);
    glVertexAttribPointer(normalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 3));

    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 6));

    // BARDZO WAŻNE: Odwiązujemy bufory tutaj.
    // Zostaną one ponownie związane w pętli renderowania.
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    return true;
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);

    GLint rotXLoc = glGetUniformLocation(program, "rotX");
    GLint rotYLoc = glGetUniformLocation(program, "rotY");
    glUniform1f(rotXLoc, rotX);
    glUniform1f(rotYLoc, rotY);
    
    // BARDZO WAŻNE: Wiazemy bufory tutaj, przed rysowaniem
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    
    // Rysujemy elementy z bufora indeksow
    glDrawElements(GL_TRIANGLES, mesh.indices.size(), GL_UNSIGNED_INT, (void*)0);
    
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
    return 0;
}
