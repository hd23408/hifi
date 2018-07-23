//
//  Created by Sabrina Shanman 7/16/2018
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "CollisionPick.h"

#include <QtCore/QDebug>

#include <glm/gtx/transform.hpp>

#include "ScriptEngineLogging.h"
#include "model-networking/ModelCache.h"

bool CollisionPick::isShapeInfoReady(CollisionRegion& pick) {
    if (pick.shouldComputeShapeInfo()) {
        if (!_cachedResource || _cachedResource->getURL() != pick.modelURL) {
            _cachedResource = DependencyManager::get<ModelCache>()->getCollisionGeometryResource(pick.modelURL);
        }

        if (_cachedResource->isLoaded()) {
            computeShapeInfo(pick, pick.shapeInfo, _cachedResource);
        }

        return false;
    }

    return true;
}

void CollisionPick::computeShapeInfo(CollisionRegion& pick, ShapeInfo& shapeInfo, QSharedPointer<GeometryResource> resource) {
    // This code was copied and modified from RenderableModelEntityItem::computeShapeInfo
    // TODO: Move to some shared code area (in entities-renderer? model-networking?)
    // after we verify this is working and do a diff comparison with RenderableModelEntityItem::computeShapeInfo
    // to consolidate the code.
    // We may also want to make computeShapeInfo always abstract away from the gpu model mesh, like it does here.
    const uint32_t TRIANGLE_STRIDE = 3;
    const uint32_t QUAD_STRIDE = 4;

    ShapeType type = shapeInfo.getType();
    glm::vec3 dimensions = pick.transform.getScale();
    if (type == SHAPE_TYPE_COMPOUND) {
        // should never fall in here when collision model not fully loaded
        // TODO: assert that all geometries exist and are loaded
        //assert(_model && _model->isLoaded() && _compoundShapeResource && _compoundShapeResource->isLoaded());
        const FBXGeometry& collisionGeometry = resource->getFBXGeometry();

        ShapeInfo::PointCollection& pointCollection = shapeInfo.getPointCollection();
        pointCollection.clear();
        uint32_t i = 0;

        // the way OBJ files get read, each section under a "g" line is its own meshPart.  We only expect
        // to find one actual "mesh" (with one or more meshParts in it), but we loop over the meshes, just in case.
        foreach (const FBXMesh& mesh, collisionGeometry.meshes) {
            // each meshPart is a convex hull
            foreach (const FBXMeshPart &meshPart, mesh.parts) {
                pointCollection.push_back(QVector<glm::vec3>());
                ShapeInfo::PointList& pointsInPart = pointCollection[i];

                // run through all the triangles and (uniquely) add each point to the hull
                uint32_t numIndices = (uint32_t)meshPart.triangleIndices.size();
                // TODO: assert rather than workaround after we start sanitizing FBXMesh higher up
                //assert(numIndices % TRIANGLE_STRIDE == 0);
                numIndices -= numIndices % TRIANGLE_STRIDE; // WORKAROUND lack of sanity checking in FBXReader

                for (uint32_t j = 0; j < numIndices; j += TRIANGLE_STRIDE) {
                    glm::vec3 p0 = mesh.vertices[meshPart.triangleIndices[j]];
                    glm::vec3 p1 = mesh.vertices[meshPart.triangleIndices[j + 1]];
                    glm::vec3 p2 = mesh.vertices[meshPart.triangleIndices[j + 2]];
                    if (!pointsInPart.contains(p0)) {
                        pointsInPart << p0;
                    }
                    if (!pointsInPart.contains(p1)) {
                        pointsInPart << p1;
                    }
                    if (!pointsInPart.contains(p2)) {
                        pointsInPart << p2;
                    }
                }

                // run through all the quads and (uniquely) add each point to the hull
                numIndices = (uint32_t)meshPart.quadIndices.size();
                // TODO: assert rather than workaround after we start sanitizing FBXMesh higher up
                //assert(numIndices % QUAD_STRIDE == 0);
                numIndices -= numIndices % QUAD_STRIDE; // WORKAROUND lack of sanity checking in FBXReader

                for (uint32_t j = 0; j < numIndices; j += QUAD_STRIDE) {
                    glm::vec3 p0 = mesh.vertices[meshPart.quadIndices[j]];
                    glm::vec3 p1 = mesh.vertices[meshPart.quadIndices[j + 1]];
                    glm::vec3 p2 = mesh.vertices[meshPart.quadIndices[j + 2]];
                    glm::vec3 p3 = mesh.vertices[meshPart.quadIndices[j + 3]];
                    if (!pointsInPart.contains(p0)) {
                        pointsInPart << p0;
                    }
                    if (!pointsInPart.contains(p1)) {
                        pointsInPart << p1;
                    }
                    if (!pointsInPart.contains(p2)) {
                        pointsInPart << p2;
                    }
                    if (!pointsInPart.contains(p3)) {
                        pointsInPart << p3;
                    }
                }

                if (pointsInPart.size() == 0) {
                    qCDebug(scriptengine) << "Warning -- meshPart has no faces";
                    pointCollection.pop_back();
                    continue;
                }
                ++i;
            }
        }

        // We expect that the collision model will have the same units and will be displaced
        // from its origin in the same way the visual model is.  The visual model has
        // been centered and probably scaled.  We take the scaling and offset which were applied
        // to the visual model and apply them to the collision model (without regard for the
        // collision model's extents).

        glm::vec3 scaleToFit = dimensions / resource->getFBXGeometry().getUnscaledMeshExtents().size();
        // multiply each point by scale
        for (int32_t i = 0; i < pointCollection.size(); i++) {
            for (int32_t j = 0; j < pointCollection[i].size(); j++) {
                // back compensate for registration so we can apply that offset to the shapeInfo later
                pointCollection[i][j] = scaleToFit * pointCollection[i][j];
            }
        }
        shapeInfo.setParams(type, dimensions, resource->getURL().toString());
    } else if (type >= SHAPE_TYPE_SIMPLE_HULL && type <= SHAPE_TYPE_STATIC_MESH) {
        const FBXGeometry& fbxGeometry = resource->getFBXGeometry();
        int numFbxMeshes = fbxGeometry.meshes.size();
        int totalNumVertices = 0;
        glm::mat4 invRegistrationOffset = glm::translate(dimensions * (-ENTITY_ITEM_DEFAULT_REGISTRATION_POINT));
        for (int i = 0; i < numFbxMeshes; i++) {
            const FBXMesh& mesh = fbxGeometry.meshes.at(i);
            totalNumVertices += mesh.vertices.size();
        }
        const int32_t MAX_VERTICES_PER_STATIC_MESH = 1e6;
        if (totalNumVertices > MAX_VERTICES_PER_STATIC_MESH) {
            qWarning() << "model" << resource->getURL() << "has too many vertices" << totalNumVertices << "and will collide as a box.";
            shapeInfo.setParams(SHAPE_TYPE_BOX, 0.5f * dimensions);
            return;
        }

        auto& meshes = resource->getFBXGeometry().meshes;
        int32_t numMeshes = (int32_t)(meshes.size());

        const int MAX_ALLOWED_MESH_COUNT = 1000;
        if (numMeshes > MAX_ALLOWED_MESH_COUNT) {
            // too many will cause the deadlock timer to throw...
            shapeInfo.setParams(SHAPE_TYPE_BOX, 0.5f * dimensions);
            return;
        }

        ShapeInfo::PointCollection& pointCollection = shapeInfo.getPointCollection();
        pointCollection.clear();
        if (type == SHAPE_TYPE_SIMPLE_COMPOUND) {
            pointCollection.resize(numMeshes);
        } else {
            pointCollection.resize(1);
        }

        ShapeInfo::TriangleIndices& triangleIndices = shapeInfo.getTriangleIndices();
        triangleIndices.clear();

        Extents extents;
        int32_t meshCount = 0;
        int32_t pointListIndex = 0;
        for (auto& mesh : meshes) {
            if (!mesh.vertices.size()) {
                continue;
            }
            QVector<glm::vec3> vertices = mesh.vertices;

            ShapeInfo::PointList& points = pointCollection[pointListIndex];

            // reserve room
            int32_t sizeToReserve = (int32_t)(vertices.count());
            if (type == SHAPE_TYPE_SIMPLE_COMPOUND) {
                // a list of points for each mesh
                pointListIndex++;
            } else {
                // only one list of points
                sizeToReserve += (int32_t)points.size();
            }
            points.reserve(sizeToReserve);

            // copy points
            uint32_t meshIndexOffset = (uint32_t)points.size();
            const glm::vec3* vertexItr = vertices.cbegin();
            while (vertexItr != vertices.cend()) {
                glm::vec3 point = *vertexItr;
                points.push_back(point);
                extents.addPoint(point);
                ++vertexItr;
            }

            if (type == SHAPE_TYPE_STATIC_MESH) {
                // copy into triangleIndices
                size_t triangleIndicesCount = 0;
                for (const FBXMeshPart& meshPart : mesh.parts) {
                    triangleIndicesCount += meshPart.triangleIndices.count();
                }
                triangleIndices.reserve((int)triangleIndicesCount);

                for (const FBXMeshPart& meshPart : mesh.parts) {
                    const int* indexItr = meshPart.triangleIndices.cbegin();
                    while (indexItr != meshPart.triangleIndices.cend()) {
                        triangleIndices.push_back(*indexItr);
                        ++indexItr;
                    }
                }
            } else if (type == SHAPE_TYPE_SIMPLE_COMPOUND) {
                // for each mesh copy unique part indices, separated by special bogus (flag) index values
                for (const FBXMeshPart& meshPart : mesh.parts) {
                    // collect unique list of indices for this part
                    std::set<int32_t> uniqueIndices;
                    auto numIndices = meshPart.triangleIndices.count();
                    // TODO: assert rather than workaround after we start sanitizing FBXMesh higher up
                    //assert(numIndices% TRIANGLE_STRIDE == 0);
                    numIndices -= numIndices % TRIANGLE_STRIDE; // WORKAROUND lack of sanity checking in FBXReader

                    auto indexItr = meshPart.triangleIndices.cbegin();
                    while (indexItr != meshPart.triangleIndices.cend()) {
                        uniqueIndices.insert(*indexItr);
                        ++indexItr;
                    }

                    // store uniqueIndices in triangleIndices
                    triangleIndices.reserve(triangleIndices.size() + (int32_t)uniqueIndices.size());
                    for (auto index : uniqueIndices) {
                        triangleIndices.push_back(index);
                    }
                    // flag end of part
                    triangleIndices.push_back(END_OF_MESH_PART);
                }
                // flag end of mesh
                triangleIndices.push_back(END_OF_MESH);
            }
            ++meshCount;
        }

        // scale and shift
        glm::vec3 extentsSize = extents.size();
        glm::vec3 scaleToFit = dimensions / extentsSize;
        for (int32_t i = 0; i < 3; ++i) {
            if (extentsSize[i] < 1.0e-6f) {
                scaleToFit[i] = 1.0f;
            }
        }
        for (auto points : pointCollection) {
            for (int32_t i = 0; i < points.size(); ++i) {
                points[i] = (points[i] * scaleToFit);
            }
        }

        shapeInfo.setParams(type, 0.5f * dimensions, resource->getURL().toString());
    }
}

CollisionRegion CollisionPick::getMathematicalPick() const {
    return _mathPick;
}

PickResultPointer CollisionPick::getEntityIntersection(const CollisionRegion& pick) {
    if (!isShapeInfoReady(*const_cast<CollisionRegion*>(&pick))) {
        // Cannot compute result
        return std::make_shared<CollisionPickResult>();
    }

    auto entityCollisionCallback = AllObjectMotionStatesCallback<EntityMotionState>(pick.shapeInfo, pick.transform);
    btCollisionWorld* collisionWorld = const_cast<btCollisionWorld*>(_collisionWorld);
    collisionWorld->contactTest(&entityCollisionCallback.collisionObject, entityCollisionCallback);

    return std::make_shared<CollisionPickResult>(pick, entityCollisionCallback.intersectingObjects, std::vector<CollisionPickResult::EntityIntersection>());
}

PickResultPointer CollisionPick::getOverlayIntersection(const CollisionRegion& pick) {
    return getDefaultResult(QVariantMap());
}

PickResultPointer CollisionPick::getAvatarIntersection(const CollisionRegion& pick) {
    if (!isShapeInfoReady(*const_cast<CollisionRegion*>(&pick))) {
        // Cannot compute result
        return std::make_shared<CollisionPickResult>();
    }

    auto avatarCollisionCallback = AllObjectMotionStatesCallback<AvatarMotionState>(pick.shapeInfo, pick.transform);
    btCollisionWorld* collisionWorld = const_cast<btCollisionWorld*>(_collisionWorld);
    collisionWorld->contactTest(&avatarCollisionCallback.collisionObject, avatarCollisionCallback);

    return std::make_shared<CollisionPickResult>(pick, std::vector<CollisionPickResult::EntityIntersection>(), avatarCollisionCallback.intersectingObjects);
}

PickResultPointer CollisionPick::getHUDIntersection(const CollisionRegion& pick) {
    return getDefaultResult(QVariantMap());
}

template <typename T = ObjectMotionState>
RigidBodyFilterResultCallback::RigidBodyFilterResultCallback(const ShapeInfo& shapeInfo, const Transform& transform) :
    btCollisionWorld::ContactResultCallback(), collisionObject() {
    const btCollisionShape* collisionShape = ObjectMotionState::getShapeManager()->getShape(shapeInfo);

    collisionObject.setCollisionShape(const_cast<btCollisionShape*>(collisionShape));

    btTransform bulletTransform;
    bulletTransform.setOrigin(glmToBullet(transform.getTranslation()));
    bulletTransform.setRotation(glmToBullet(transform.getRotation()));

    collisionObject.setWorldTransform(bulletTransform);
}

template <typename T = ObjectMotionState>
RigidBodyFilterResultCallback::~RigidBodyFilterResultCallback() {
    ObjectMotionState::getShapeManager()->releaseShape(collisionObject.getCollisionShape());
}

template <typename T = ObjectMotionState>
btScalar RigidBodyFilterResultCallback::addSingleResult(btManifoldPoint& cp, const btCollisionObjectWrapper* colObj0, int partId0, int index0, const btCollisionObjectWrapper* colObj1, int partId1, int index1) {
    const btCollisionObject* otherBody;
    btVector3 point;
    btVector3 otherPoint;
    if (colObj0->m_collisionObject == &collisionObject) {
        otherBody = colObj1->m_collisionObject;
        point = cp.m_localPointA;
        otherPoint = cp.m_localPointB;
    }
    else {
        otherBody = colObj0->m_collisionObject;
        point = cp.m_localPointB;
        otherPoint = cp.m_localPointA;
    }
    const btRigidBody* collisionCandidate = dynamic_cast<const btRigidBody*>(otherBody);
    if (!collisionCandidate) {
        return 0;
    }
    const btMotionState* motionStateCandidate = collisionCandidate->getMotionState();

    checkOrAddCollidingState(motionStateCandidate, point, otherPoint);

    return 0;
}

template <typename T = ObjectMotionState>
void AllObjectMotionStatesCallback::checkOrAddCollidingState(const btMotionState* otherMotionState, btVector3& point, btVector3& otherPoint) {
    const T* candidate = dynamic_cast<const T*>(otherMotionState);
    if (!candidate) {
        return;
    }

    // This is the correct object type. Add it to the list.
    intersectingObjects.emplace_back(candidate->getObjectID(), bulletToGLM(point), bulletToGLM(otherPoint));
}