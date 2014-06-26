//
//  MetavoxelClientManager.h
//  libraries/metavoxels/src
//
//  Created by Andrzej Kapolka on 6/26/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_MetavoxelClientManager_h
#define hifi_MetavoxelClientManager_h

#include "Endpoint.h"

class MetavoxelEditMessage;

/// Manages the set of connected metavoxel clients.
class MetavoxelClientManager : public QObject {
    Q_OBJECT

public:

    virtual void init();
    virtual void update();

    SharedObjectPointer findFirstRaySpannerIntersection(const glm::vec3& origin, const glm::vec3& direction,
        const AttributePointer& attribute, float& distance);
        
    Q_INVOKABLE void applyEdit(const MetavoxelEditMessage& edit, bool reliable = false);

    virtual MetavoxelLOD getLOD() const;
    
private slots:

    void maybeAttachClient(const SharedNodePointer& node);
};

/// Base class for metavoxel clients.
class MetavoxelClient : public Endpoint {
    Q_OBJECT

public:
    
    MetavoxelClient(const SharedNodePointer& node, MetavoxelClientManager* manager);

    MetavoxelData& getData() { return _data; }

    void guide(MetavoxelVisitor& visitor);
    
    void applyEdit(const MetavoxelEditMessage& edit, bool reliable = false);

protected:

    virtual void writeUpdateMessage(Bitstream& out);
    virtual void readMessage(Bitstream& in);
    virtual void handleMessage(const QVariant& message, Bitstream& in);

    virtual PacketRecord* maybeCreateSendRecord(bool baseline) const;
    virtual PacketRecord* maybeCreateReceiveRecord(bool baseline) const;

private:
    
    MetavoxelClientManager* _manager;
    MetavoxelData _data;
};

#endif // hifi_MetavoxelClientManager_h
