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
float rotX=0, rotY=0;
bool mouseDown=false;
int lastX, lastY;
struct Mesh {
    std::vector<float> vertices; // 3 floats per vertex
    std::vector<unsigned int> indices;
};

Mesh mesh;
GLuint program, vbo, ibo;
float angle = 0.0f;

const char* vs = R"(
attribute vec3 aPos;
attribute vec2 aUV;
attribute vec3 aNormal;

varying vec2 vUV;
varying vec3 vNormal;

uniform float rotX, rotY;

void main(){
    float cx = cos(rotX), sx=sin(rotX);
    float cy = cos(rotY), sy=sin(rotY);
    mat3 Rx = mat3(1,0,0, 0,cx,-sx, 0,sx,cx);
    mat3 Ry = mat3(cy,0,sy, 0,1,0, -sy,0,cy);
    vec3 p = Ry * Rx * aPos;
    gl_Position = vec4(p * 0.5 + vec3(0.0, 0.0, -1.0), 1.0);

    vUV = aUV;
    vNormal = normalize(Ry * Rx * aNormal);
}
)";

const char* fs = R"(
precision mediump float;
void main() {
    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); // Rysuj na czerwono
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

    const aiMesh* meshData = scene->mMeshes[0];

    // Zbieramy wszystkie atrybuty w jednym buforze, tak jest wydajniej
    // 3x pozycja, 2x UV, 3x normalna = 8 floatow na wierzcholek
    m.vertices.resize(meshData->mNumVertices * 8);

    for (unsigned int i = 0; i < meshData->mNumVertices; ++i) {
        // Pozycja (3 floats)
        m.vertices[i * 8 + 0] = meshData->mVertices[i].x;
        m.vertices[i * 8 + 1] = meshData->mVertices[i].y;
        m.vertices[i * 8 + 2] = meshData->mVertices[i].z;

        // Normalne (3 floats)
        if(meshData->HasNormals()){
            m.vertices[i * 8 + 3] = meshData->mNormals[i].x;
            m.vertices[i * 8 + 4] = meshData->mNormals[i].y;
            m.vertices[i * 8 + 5] = meshData->mNormals[i].z;
        }

        // UV (2 floats)
        if(meshData->HasTextureCoords(0)){
            m.vertices[i * 8 + 6] = meshData->mTextureCoords[0][i].x;
            m.vertices[i * 8 + 7] = meshData->mTextureCoords[0][i].y;
        }
    }

    for (unsigned int i = 0; i < meshData->mNumFaces; ++i) {
        const aiFace& face = meshData->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            m.indices.push_back(face.mIndices[j]);
        }
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

    mesh = loadMeshFromAssimp("asserts/Earth 2K.fbx");
    printf("Vertices: %zu, Indices: %zu\n", mesh.vertices.size()/3, mesh.indices.size()/3);

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    // Zmien w init()
// ...
glGenBuffers(1, &vbo);
glBindBuffer(GL_ARRAY_BUFFER, vbo);
// Dane teraz mają 8 floatów na wierzchołek
glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size()*sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);

glGenBuffers(1, &ibo);
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size()*sizeof(unsigned int), mesh.indices.data(), GL_STATIC_DRAW);

glUseProgram(program); // Uzywaj programu, aby pobrac lokalizacje atrybutow

// Konfiguracja atrybutow (ZMIENIONO)
GLint posLoc = glGetAttribLocation(program, "aPos");
GLint normalLoc = glGetAttribLocation(program, "aNormal");
GLint uvLoc = glGetAttribLocation(program, "aUV");

// Sprawdz czy atrybuty istnieja (warto dodac diagnostyke)
printf("aPos location: %d, aNormal location: %d, aUV location: %d\n", posLoc, normalLoc, uvLoc);

// Pozycja (offset 0, 3 floata)
glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)0);
glEnableVertexAttribArray(posLoc);

// Normalne (offset 3, 3 floaty)
glVertexAttribPointer(normalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 3));
glEnableVertexAttribArray(normalLoc);

// UV (offset 6, 2 floaty)
glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 6));
glEnableVertexAttribArray(uvLoc);
// ...
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
