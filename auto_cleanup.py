import oci
import subprocess

# Step 1: Ask for compartment name
compartment_name = input("Enter the compartment name (e.g., OCI-LAB-35): ").strip()

# Step 2: Load OCI config and get compartment OCID
config = oci.config.from_file()
identity = oci.identity.IdentityClient(config)

# Get list of compartments (including root)
compartments = oci.pagination.list_call_get_all_results(
    identity.list_compartments,
    config["tenancy"],
    compartment_id_in_subtree=True
).data
compartments.append(identity.get_compartment(config["tenancy"]).data)

# Find compartment OCID
compartment_ocid = None
for c in compartments:
    if c.name == compartment_name and c.lifecycle_state == "ACTIVE":
        compartment_ocid = c.id
        break

if not compartment_ocid:
    print(f"❌ Compartment '{compartment_name}' not found or not active.")
    exit(1)

print(f"🔎 Compartment OCID: {compartment_ocid}")

# Step 3: Find regions with resources using Resource Search
regions = identity.list_region_subscriptions(config["tenancy"]).data
resource_search_client = oci.resource_search.ResourceSearchClient(config)
found_regions = []

for region in regions:
    config["region"] = region.region_name
    try:
        result = resource_search_client.search_resources(
            search_details=oci.resource_search.models.StructuredSearchDetails(
                query=f"query all resources where compartmentId = '{compartment_ocid}'",
                type="Structured"
            ),
            limit=1
        ).data.items

        if result:
            print(f"✅ Resources found in {region.region_name}")
            found_regions.append(region.region_name)
        else:
            print(f"❌ No resources in {region.region_name}")
    except Exception as e:
        print(f"⚠️ Error in {region.region_name}: {e}")

# Step 4: Construct region list and run cleanup
if not found_regions:
    print("🚫 No regions with resources found — cleanup not required.")
    exit(0)

region_list = ",".join(found_regions)
cleanup_cmd = ["./cleanup.py", "-c", compartment_name, "-r", region_list]

print("\n🧹 Running cleanup with:")
print(" ".join(cleanup_cmd))

try:
    subprocess.run(cleanup_cmd, check=True)
except subprocess.CalledProcessError as e:
    print(f"❌ Cleanup script failed: {e}")
