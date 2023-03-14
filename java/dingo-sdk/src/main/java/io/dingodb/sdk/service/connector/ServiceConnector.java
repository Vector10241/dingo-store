/*
 * Copyright 2021, Zetyun DataPortal All rights reserved.
 */

package io.dingodb.sdk.service.connector;

import io.dingodb.common.Common;
import io.dingodb.coordinator.Coordinator;
import io.dingodb.coordinator.CoordinatorServiceGrpc;
import io.dingodb.sdk.common.utils.GrpcConnection;
import io.dingodb.meta.MetaServiceGrpc;
import io.grpc.ManagedChannel;
import lombok.Getter;

public class ServiceConnector {

    private String target;
    @Getter
    private MetaServiceGrpc.MetaServiceBlockingStub metaBlockingStub;

    public ServiceConnector(String target) {
        this.target = target;
    }

    public void initConnection() {
        ManagedChannel channel = GrpcConnection.newChannel(target);
        CoordinatorServiceGrpc.CoordinatorServiceBlockingStub blockingStub =
                CoordinatorServiceGrpc.newBlockingStub(channel);
        Coordinator.GetCoordinatorMapResponse response = blockingStub.getCoordinatorMap(
                Coordinator.GetCoordinatorMapRequest.newBuilder().setClusterId(0).build());

        Common.Location leaderLocation = response.getLeaderLocation();
        if (!leaderLocation.getHost().isEmpty()) {
            target = leaderLocation.getHost() + ":" + leaderLocation.getPort();
            channel = GrpcConnection.newChannel(target);
            metaBlockingStub = MetaServiceGrpc.newBlockingStub(channel);
        }

    }
}
