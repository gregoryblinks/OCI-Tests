#!/usr/bin/env python3
import oci
import subprocess

# Step 1: Ask for compartment name
compartment_name = input("Enter the compartment name (e.g., OCI-LAB-##): ").strip()

# Step 2: Load OCI config and get compartment OCID
config = oci.config.from_file()
identity = oci.identity.IdentityClient(config)
resource_search_client = oci.resource_search.ResourceSearchClient(config)

# Get compartments (including root)
compartments = oci.pagination.list_call_get_all_results(
    identity.list_compartments,
    config["tenancy"],
    compartment_id_in_subtree=True
).data
compartments.append(identity.get_compartment(config["tenancy"]).data)

compartment_ocid = None
for c in compartments:
    if c.name == compartment_name and c.lifecycle_state == "ACTIVE":
        compartment_ocid = c.id
        break

if not compartment_ocid:
    print(f"❌ Compartment '{compartment_name}' not found or not active.")
    exit(1)

print(f"🔎 Compartment OCID: {compartment_ocid}")

# Step 3: Run ociLabMgmt.py
oci_lab_cmd = ["./ociLabMgmt.py", "--delete", "--compartment", compartment_name]
print("\n🧹 Running ociLabMgmt cleanup:")
print(" ".join(oci_lab_cmd))
try:
    subprocess.run(oci_lab_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ ociLabMgmt.py failed: {e}")
    exit(1)

# Step 4: Prepare billable resource list (approximate to Tenancy Explorer logic)
important_billable_resources = {
    # Compute & Storage
    "Instance", "BootVolume", "Volume", "Image", "InstancePool",
    "VolumeBackup", "BootVolumeBackup", "VolumeGroup",

    # Database
    "DbSystem", "AutonomousDatabase", "AutonomousDatabaseBackup",

    # Load Balancer
    "LoadBalancer",

    # Object / File Storage
    "Bucket", "FileSystem", "MountTarget",

    # Streaming
    "Stream", "StreamPool",

    # Vault & Security
    "Vault", "Key", "Secret",

    # Containers
    "Cluster", "NodePool",

    # Analytics / Integration
    "AnalyticsInstance", "IntegrationInstance",

    # Serverless & APIs
    "Function", "ApiGateway", "ApiDeployment",

    # Monitoring / Logging
    "Alarm", "LogGroup", "Log",

    # Networking (billable-impacting infra)
    "Vcn", "Subnet", "Drg", "InternetGateway",
    "NatGateway", "ServiceGateway", "RouteTable", "SecurityList",

    # Access & Automation
    "ServiceConnector", "Bastion"
}

# Get supported resource types from OCI
try:
    supported_resource_types = [t.name for t in resource_search_client.list_resource_types().data]
except Exception as e:
    print(f"❌ Failed to fetch supported resource types: {e}")
    exit(1)

supported_lower = {t.lower(): t for t in supported_resource_types}

# Match our list to valid OCI names
searchable_types = []
print("\n📋 Resource type mapping (requested → OCI name / status):")
for rtype in sorted(important_billable_resources):
    rtype_lower = rtype.lower()
    if rtype_lower in supported_lower:
        oci_name = supported_lower[rtype_lower]
        searchable_types.append(oci_name)
        print(f"  ✅ {rtype} → {oci_name}")
    else:
        print(f"  ⚠️ {rtype} → Unsupported in Resource Search")

if not searchable_types:
    print("🚫 No searchable resource types found — aborting.")
    exit(1)

# Step 5: Search resources in subscribed regions
regions = identity.list_region_subscriptions(config["tenancy"]).data
found_regions = []

print("\n🌍 Checking regions for active billable resources...")
for region in regions:
    config["region"] = region.region_name
    resource_search_client = oci.resource_search.ResourceSearchClient(config)
    found_in_region = False

    for rtype in searchable_types:
        query = f"query {rtype} resources where compartmentId = '{compartment_ocid}'"
        try:
            result = resource_search_client.search_resources(
                search_details=oci.resource_search.models.StructuredSearchDetails(
                    query=query,
                    type="Structured"
                ),
                limit=10
            ).data.items

            for item in result:
                state = getattr(item, "lifecycle_state", None)
                if not state or state.upper() != "TERMINATED":  # case-insensitive filter
                    print(f"✅ {region.region_name}: {item.resource_type} - {item.display_name} ({state})")
                    found_in_region = True
                    break

        except oci.exceptions.ServiceError as e:
            print(f"⚠️ OCI Service Error in {region.region_name} for {rtype}: {e.code} - {e.message}")
        except Exception as e:
            print(f"⚠️ Unexpected error in {region.region_name} for {rtype}: {e}")

    if found_in_region:
        found_regions.append(region.region_name)
    else:
        print(f"❌ No active resources found in {region.region_name}")

# Step 6: Cleanup
if not found_regions:
    print("🚫 No active regions with resources found — skipping cleanup.py.")
    exit(0)

region_list = ",".join(found_regions)
cleanup_cmd = ["./cleanup.py", "-c", compartment_name, "-r", region_list]

print("\n🧹 Running cleanup.py:")
print(" ".join(cleanup_cmd))
try:
    subprocess.run(cleanup_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ cleanup.py failed: {e}")
